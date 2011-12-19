#include "stdafx.h"
#include "log_manager.h"

#include "writer.h"

#include "../misc/pattern_formatter.h"
#include "../misc/config.h"

#include "../ytree/serialize.h"
#include "../ytree/ypath_client.h"
#include "../ytree/ypath_service.h"

#include <util/folder/dirut.h>

namespace NYT {
namespace NLog {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

// TODO: review this and that
static const char* const SystemPattern = "$(datetime) $(level) $(category) $(message)";

static const char* const DefaultStdErrWriterName = "StdErr";
static const ELogLevel DefaultStdErrMinLevel= ELogLevel::Info;
static const char* const DefaultStdErrPattern = "$(datetime) $(level) $(category) $(message)";

static const char* const DefaultFileWriterName = "LogFile";
static const char* const DefaultFileName = "default.log";
static const ELogLevel DefaultFileMinLevel = ELogLevel::Debug;
static const char* const DefaultFilePattern =
    "$(datetime) $(level) $(category) $(message)$(tab)$(file?) $(line?) $(function?) $(thread?)";

static const char* const AllCategoriesName = "*";

static TLogger Logger(SystemLoggingCategory);

////////////////////////////////////////////////////////////////////////////////

struct TRule
    : public TConfigBase
{
    typedef TIntrusivePtr<TRule> TPtr;

    bool AllCategories;
    yhash_set<Stroka> Categories;

    ELogLevel MinLevel;
    ELogLevel MaxLevel;

    yvector<Stroka> Writers;
    
    TRule()
        : AllCategories(false)
    {
        Register("categories", Categories).NonEmpty();
        Register("min_level", MinLevel).Default(ELogLevel::Minimum);
        Register("max_level", MaxLevel).Default(ELogLevel::Maximum);
        Register("writers", Writers).NonEmpty();
    }

    virtual void Load(NYTree::INode* node, const NYTree::TYPath& path)
    {
        TConfigBase::Load(node, path);

        if (Categories.size() == 1 && *Categories.begin() == AllCategoriesName) {
            AllCategories = true;
        }
    }

    bool IsApplicable(const Stroka& category) const
    {
        return AllCategories || Categories.find(category) != Categories.end();
    }

    bool IsApplicable(const Stroka& category, ELogLevel level) const
    {
        return
            MinLevel <= level && level <= MaxLevel &&
            IsApplicable(category);
    }
};

////////////////////////////////////////////////////////////////////////////////

typedef yvector<ILogWriter::TPtr> TLogWriters;

////////////////////////////////////////////////////////////////////////////////

class TLogConfig
    : public TConfigBase
{
public:
    typedef TIntrusivePtr<TLogConfig> TPtr;
    
    /*
     *  Needs to be public for TConfigBase.
     *  Not for user
     */
    TLogConfig()
    {
        Register("writers", WriterConfigs);
        Register("rules", Rules);
    }

    TLogWriters GetWriters(const TLogEvent& event)
    {
        TPair<Stroka, ELogLevel> cacheKey(event.Category, event.Level);
        auto it = CachedWriters.find(cacheKey);
        if (it != CachedWriters.end())
            return it->second;
    
        yhash_set<Stroka> writerIds;
        FOREACH(auto& rule, Rules) {
            if (rule->IsApplicable(event.Category, event.Level)) {
                writerIds.insert(rule->Writers.begin(), rule->Writers.end());
            }
        }

        TLogWriters writers;
        FOREACH(const Stroka& writerId, writerIds) {
            auto writerIt = Writers.find(writerId);
            YASSERT(writerIt != Writers.end());
            writers.push_back(writerIt->second);
        }

        YVERIFY(CachedWriters.insert(MakePair(cacheKey, writers)).second);

        return writers;
    }

    ELogLevel GetMinLevel(Stroka category) const
    {
        ELogLevel level = ELogLevel::Maximum;
        FOREACH (const auto& rule, Rules) {
            if (rule->IsApplicable(category)) {
                level = Min(level, rule->MinLevel);
            }
        }
        return level;
    }

    TVoid FlushWriters()
    {
        FOREACH(auto& pair, Writers) {
            pair.second->Flush();
        }
        return TVoid();
    }

    static TPtr CreateDefault()
    {
        auto config = New<TLogConfig>();

        config->Writers.insert(
            MakePair(DefaultStdErrWriterName, New<TStdErrLogWriter>(SystemPattern)));
        
        config->Writers.insert(
            MakePair(DefaultFileWriterName, New<TFileLogWriter>(DefaultFileName, DefaultFilePattern)));

        auto stdErrRule = New<TRule>();
        stdErrRule->AllCategories = true;
        stdErrRule->MinLevel = DefaultStdErrMinLevel;
        stdErrRule->Writers.push_back(DefaultStdErrWriterName);
        config->Rules.push_back(stdErrRule);

        auto fileRule = New<TRule>();
        fileRule->AllCategories = true;
        fileRule->MinLevel = DefaultFileMinLevel;
        fileRule->Writers.push_back(DefaultFileWriterName);
        config->Rules.push_back(fileRule);

        return config;
    }

    static TPtr CreateFromNode(INode* node, const TYPath& path = "/")
    {
        auto config = New<TLogConfig>();
        config->Load(node, path);
        config->Validate(path);
        config->CreateWriters();
        return config;
    }

private:
    virtual void Validate(const NYTree::TYPath& path) const
    {
        TConfigBase::Validate(path);

        FOREACH (const auto& rule, Rules) {
            FOREACH (const Stroka& writer, rule->Writers) {
                if (WriterConfigs.find(writer) == WriterConfigs.end()) {
                    ythrow yexception() <<
                        Sprintf("Writer %s was not defined (Path: %s)",
                            ~writer,
                            ~path);
                }
            }
        }
    }

    void CreateWriters()
    {
        FOREACH(const auto& pair, WriterConfigs) {
            const auto& name = pair.first;
            const auto& config = pair.second;
            const auto& pattern = config->Pattern;
            switch (config->Type) {
                case ILogWriter::EType::File:
                    YVERIFY(
                        Writers.insert(MakePair(
                            name, New<TFileLogWriter>(config->FileName, pattern))).Second());
                    break;
                case ILogWriter::EType::StdOut:
                    YVERIFY(
                        Writers.insert(MakePair(
                            name, New<TStdOutLogWriter>(pattern))).Second());
                    break;
                case ILogWriter::EType::StdErr:
                    YVERIFY(
                        Writers.insert(MakePair(
                            name, New<TStdErrLogWriter>(pattern))).Second());
                    break;
            }
        }
    }

    yvector<TRule::TPtr> Rules;
    yhash_map<Stroka, ILogWriter::TConfig::TPtr> WriterConfigs;
    yhash_map<Stroka, ILogWriter::TPtr> Writers;
    ymap<TPair<Stroka, ELogLevel>, TLogWriters> CachedWriters;
};

////////////////////////////////////////////////////////////////////////////////

class TLogManager::TImpl
{
public:
    TImpl()
        : ConfigVersion(0)
        , Config(TLogConfig::CreateDefault())
        , Queue(New<TActionQueue>("LogManager", false))
    {
        SystemWriters.push_back(New<TStdErrLogWriter>(SystemPattern));
    }

    void Configure(NYTree::INode* node, const NYTree::TYPath& path = "/")
    {
        auto config = TLogConfig::CreateFromNode(node, path);
        auto queue = Queue;
        if (~queue != NULL) {
            queue->GetInvoker()->Invoke(
                FromMethod(&TImpl::UpdateConfig, this, config));
        }
    }

    void Configure(const Stroka& fileName, const NYTree::TYPath& path)
    {
        try {
            LOG_TRACE("Configuring logging (FileName: %s, Path: %s)", ~fileName, ~path);
            TIFStream configStream(fileName);
            auto root = DeserializeFromYson(&configStream);
            auto rootService = IYPathService::FromNode(~root);
            auto configNode = SyncYPathGetNode(~rootService, path);
            Configure(~configNode, path);
        } catch (const yexception& ex) {
            LOG_ERROR("Error configuring logging\n%s", ex.what())
        }
    }

    void Flush()
    {
        auto queue = Queue;
        if (~queue != NULL) {
            FromMethod(&TLogConfig::FlushWriters, Config)
                ->AsyncVia(queue->GetInvoker())
                ->Do()
                ->Get();
        }
    }

    void Shutdown()
    {
        Flush();
    
        auto queue = Queue;
        if (~queue != NULL) {
            Queue.Reset();
            queue->Shutdown();
        }
    }

    int GetConfigVersion()
    {
        TGuard<TSpinLock> guard(&SpinLock);
        return ConfigVersion;
    }

    void GetLoggerConfig(
        Stroka category,
        ELogLevel* minLevel,
        int* configVersion)
    {
        TGuard<TSpinLock> guard(&SpinLock);
        *minLevel = Config->GetMinLevel(category);
        *configVersion = ConfigVersion;
    }

    void Write(const TLogEvent& event)
    {
        auto queue = Queue;
        if (~queue != NULL) {
            queue->GetInvoker()->Invoke(FromMethod(&TImpl::DoWrite, this, event));

            // TODO: use system-wide exit function
            if (event.Level == ELogLevel::Fatal) {
                Shutdown();
                ::std::terminate();
            }
        }
    }

private:
    typedef yvector<ILogWriter::TPtr> TWriters;

    TWriters GetWriters(const TLogEvent& event)
    {
        if (event.Category == SystemLoggingCategory)
            return SystemWriters;

        return Config->GetWriters(event);
    }

    void DoWrite(const TLogEvent& event)
    {
        FOREACH(auto& writer, GetWriters(event)) {
            writer->Write(event);
        }
    }

    void UpdateConfig(TLogConfig::TPtr config)
    {
        config->FlushWriters();

        TGuard<TSpinLock> guard(&SpinLock);
        Config = config;
        ConfigVersion++;
    }

    // Configuration.
    TLogConfig::TPtr Config;
    TAtomic ConfigVersion;
    TSpinLock SpinLock;

    TActionQueue::TPtr Queue;

    TWriters SystemWriters;
};

////////////////////////////////////////////////////////////////////////////////

TLogManager::TLogManager()
    : Impl(new TImpl())
{ }

TLogManager* TLogManager::Get()
{
    return Singleton<TLogManager>();
}

void TLogManager::Configure(INode* node)
{
    Impl->Configure(node);
}

void TLogManager::Configure(const Stroka& fileName, const TYPath& path)
{
    Impl->Configure(fileName, path);
}

void TLogManager::Flush()
{
    Impl->Flush();
}

void TLogManager::Shutdown()
{
    Impl->Shutdown();
}

int TLogManager::GetConfigVersion()
{
    return Impl->GetConfigVersion();
}

void TLogManager::GetLoggerConfig(Stroka category, ELogLevel* minLevel, int* configVersion)
{
    Impl->GetLoggerConfig(category, minLevel, configVersion);
}

void TLogManager::Write(const TLogEvent& event)
{
    Impl->Write(event);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NLog
}  // namespace NYT
