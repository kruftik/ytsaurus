#include <yt/yt/server/lib/hydra/unbuffered_file_changelog.h>
#include <yt/yt/server/lib/hydra/serialize.h>
#include <yt/yt/server/lib/hydra/config.h>

#include <yt/yt/server/lib/io/io_engine.h>
#include <yt/yt/server/lib/io/public.h>

#include <yt/yt/core/misc/fs.h>

#include <library/cpp/getopt/last_getopt.h>

#include <fstream>

namespace NYT {

using namespace NFS;
using namespace NHydra;
using namespace NIO;
using namespace NLastGetopt;

YT_DEFINE_GLOBAL(const NLogging::TLogger, Logger, "ChangelogSurgeonLogger");
constexpr int DefaultMaxRecordsPerRead = 1000000;

////////////////////////////////////////////////////////////////////////////////

bool ValidateRecordIdentity(const TSharedRef& lhs, const TSharedRef& rhs)
{
    NYT::NHydra::NProto::TMutationHeader lhsMutationHeader;
    NYT::NHydra::NProto::TMutationHeader rhsMutationHeader;
    TSharedRef lhsMutationData;
    TSharedRef rhsMutationData;

    DeserializeMutationRecord(lhs, &lhsMutationHeader, &lhsMutationData);
    DeserializeMutationRecord(rhs, &rhsMutationHeader, &rhsMutationData);

    auto serializedLhsMutationHeader = ToString(lhsMutationHeader);
    auto serializedRhsMutationHeader = ToString(rhsMutationHeader);
    if (serializedLhsMutationHeader != serializedRhsMutationHeader) {
        YT_LOG_ERROR("Mutation headers of mutations with the same sequence number differ (FirstMutationHeader: %v, SecondMutationHeader: %v",
            serializedLhsMutationHeader,
            serializedRhsMutationHeader);
        return false;
    }

    if (TRef::AreBitwiseEqual(lhsMutationData, rhsMutationData)) {
        YT_LOG_ERROR("Mutation data of mutations with the same sequence number differ (FirstMutationHeader: %v, SecondMutationHeader: %v",
            serializedLhsMutationHeader,
            serializedRhsMutationHeader);
        return false;
    }

    return true;
}

i64 DeduceFirstSequenceNumber(const TVector<TString>& changelogFileNames)
{
    bool allRecordsEmpty = true;
    i64 seqNumber = std::numeric_limits<i64>::max();

    auto ioEngine = CreateIOEngine(EIOEngineType::ThreadPool, NYT::NYTree::INodePtr());
    auto config = New<TFileChangelogConfig>();

    for (const auto& fileName : changelogFileNames) {
        auto currentChangelog = CreateUnbufferedFileChangelog(
            ioEngine,
            /*memoryUsageTracker*/ nullptr,
            fileName,
            config);
        currentChangelog->Open();
        auto recordCount = currentChangelog->GetRecordCount();
        if (recordCount == 0) {
            continue;
        }
        allRecordsEmpty = false;

        auto records = currentChangelog->Read(0, 1, std::numeric_limits<i64>::max());
        if (records.empty()) {
            THROW_ERROR_EXCEPTION("No records were read from a changelog");
        }
        YT_VERIFY(std::ssize(records) == 1);

        NYT::NHydra::NProto::TMutationHeader mutationHeader;
        TSharedRef mutationData;

        DeserializeMutationRecord(records[0], &mutationHeader, &mutationData);

        seqNumber = std::min(seqNumber, mutationHeader.sequence_number());
    }

    if (allRecordsEmpty) {
        THROW_ERROR_EXCEPTION("Error deducing first seqenuce number, all records are empty");
    }

    YT_LOG_INFO("Deduced first sequence number for a set of changelogs (FirstSequenuceNumber: %v)",
        seqNumber);

    return seqNumber;
}

void PerformSurgery(
    i64 firstSeqNum,
    i64 lastSeqNum,
    const TVector<TString>& changelogFileNames,
    const TString& resultingChangelogId,
    int maxRecordsPerRead)
{
    std::vector<TSharedRef> resultingRecords(lastSeqNum - firstSeqNum + 1);
    std::vector<bool> found(lastSeqNum - firstSeqNum + 1);

    NYT::NHydra::NProto::TMutationHeader mutationHeader;
    TSharedRef mutationData;

    auto ioEngine = CreateIOEngine(EIOEngineType::ThreadPool, NYT::NYTree::INodePtr());
    auto config = New<TFileChangelogConfig>();

    for (const auto& fileName : changelogFileNames) {
        auto currentChangelog = CreateUnbufferedFileChangelog(
            ioEngine,
            /*memoryUsageTracker*/ nullptr,
            fileName,
            config);
        currentChangelog->Open();
        auto recordCount = currentChangelog->GetRecordCount();
        if (recordCount == 0) {
            YT_LOG_INFO("Changelog is empty (Filename: %v)",
                fileName);
            currentChangelog->Close();
            continue;
        }

        // Read a part of a changelog.
        auto recordsRead = 0;
        while (recordsRead < recordCount) {
            auto changelogRecords = currentChangelog->Read(recordsRead, maxRecordsPerRead, std::numeric_limits<i64>::max());
            if (changelogRecords.size() == 0) {
                THROW_ERROR_EXCEPTION("No records were read from a changelog");
            }

            recordsRead += changelogRecords.size();

            for (const auto& record : changelogRecords) {
                DeserializeMutationRecord(record, &mutationHeader, &mutationData);
                auto seqNumber = mutationHeader.sequence_number();
                if (firstSeqNum <= seqNumber && seqNumber <= lastSeqNum) {
                    if (!found[seqNumber - firstSeqNum]) {
                        resultingRecords[seqNumber - firstSeqNum] = record;
                        found[seqNumber - firstSeqNum] = true;
                    } else if (!ValidateRecordIdentity(record, resultingRecords[seqNumber - firstSeqNum])) {
                        return;
                    }
                }
            }
        }
        currentChangelog->Close();
    }

    ui64 prevRandomSeed = 0;
    i64 resultingId = FromString<i64>(resultingChangelogId);
    for (int i = 0; i < std::ssize(resultingRecords); ++i) {
        if (!found[i]) {
            THROW_ERROR_EXCEPTION("Mutation is missing (SequenceNumber: %v)", i + firstSeqNum);
        }

        DeserializeMutationRecord(resultingRecords[i], &mutationHeader, &mutationData);
        if (mutationHeader.prev_random_seed() != prevRandomSeed && i != 0) {
            auto secondMutationHeader = mutationHeader;
            DeserializeMutationRecord(resultingRecords[i - 1], &mutationHeader, &mutationData);
            THROW_ERROR_EXCEPTION("Random seeds do not match (FirstMutationHeader: %v, SecondMutationHeader: %v)",
                ToString(mutationHeader),
                ToString(secondMutationHeader));
        }

        if (mutationHeader.segment_id() != resultingId) {
            YT_LOG_WARNING("An original mutation segment_id was substituted with a new one (OldSegmentId: %v, NewSegmentId: %v)",
                mutationHeader.segment_id(),
                resultingId);
        }
        if (mutationHeader.record_id() != i) {
            YT_LOG_WARNING("An original mutation record_id was substituted with a new one (OldRecordId: %v, NewRecordId: %v)",
                mutationHeader.record_id(),
                i);
        }
        mutationHeader.set_segment_id(resultingId);
        mutationHeader.set_record_id(i);
        resultingRecords[i] = SerializeMutationRecord(mutationHeader, mutationData);
        prevRandomSeed = mutationHeader.random_seed();
    }

    auto resultingChangelogName = resultingChangelogId + ".log";
    std::ofstream { resultingChangelogName };
    auto resultingChangelog = CreateUnbufferedFileChangelog(
        ioEngine,
        /*memoryUsageTracker*/ nullptr,
        resultingChangelogName,
        config);
    resultingChangelog->Create(/*meta*/ {});
    resultingChangelog->Append(0, resultingRecords);
    resultingChangelog->Close();
}

void Run(int argc, char** argv)
{
    TOpts opts;

    std::optional<i64> firstSeqNum;
    opts.AddCharOption('f', "[inclusive] Start of the range of sequence numbers to be used to build a new changelog")
        .RequiredArgument("FIRST_SEQUENCE_NUMBER")
        .Optional()
        .StoreResult(&firstSeqNum);

    i64 lastSeqNum = 0;
    opts.AddCharOption('t', "[inclusive] End of the range of sequence numbers to be used to build a new changelog")
        .RequiredArgument("LAST_SEQUENCE_NUMBER")
        .Required()
        .StoreResult(&lastSeqNum);

    TString resultingChangelogId;
    opts.AddCharOption('i', "Desired output changelog name, should be convertible to a number")
        .RequiredArgument("CHANGELOG_ID")
        .Required()
        .StoreResult(&resultingChangelogId);

    TString changelogList;
    opts.AddCharOption('c', "List of all changelogs that can be used to transplant their entries to the new one")
        .RequiredArgument("CHANGELOG_LIST")
        .Required()
        .StoreResult(&changelogList);

    int maxRecordsPerRead = DefaultMaxRecordsPerRead;
    opts.AddLongOption("max-records-per-read", "Max number of records to read from a changelog at once")
        .RequiredArgument("RECORDS_PER_READ")
        .StoreResult(&maxRecordsPerRead);

    TOptsParseResult parseResult(&opts, argc, argv);

    auto changelogFileNames = StringSplitter(changelogList).Split(' ').ToList<TString>();

    if (!firstSeqNum) {
        firstSeqNum = DeduceFirstSequenceNumber(changelogFileNames);
    }

    PerformSurgery(
        *firstSeqNum,
        lastSeqNum,
        changelogFileNames,
        resultingChangelogId,
        maxRecordsPerRead);
}

} // namespace NYT

int main(int argc, char** argv)
{
    NYT::Run(argc, argv);

    return 0;
}
