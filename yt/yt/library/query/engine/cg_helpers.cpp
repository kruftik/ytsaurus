#include "cg_helpers.h"
#include "cg_fragment_compiler.h"

namespace NYT::NQueryClient {

////////////////////////////////////////////////////////////////////////////////

StringRef ToStringRef(TStringBuf stringBuf)
{
    return StringRef(stringBuf.data(), stringBuf.length());
}

StringRef ToStringRef(TRef ref)
{
    return StringRef(ref.Begin(), ref.Size());
}

////////////////////////////////////////////////////////////////////////////////

Type* GetABIType(llvm::LLVMContext& context, NYT::NTableClient::EValueType staticType)
{
    return TDataTypeBuilder::Get(context, staticType);
}

Type* GetLLVMType(llvm::LLVMContext& context, NYT::NTableClient::EValueType staticType)
{
    if (staticType == EValueType::Boolean) {
        return TTypeBuilder<NCodegen::TTypes::i<1>>::Get(context);
    }

    return GetABIType(context, staticType);
}

////////////////////////////////////////////////////////////////////////////////

Value* TCGExprContext::GetFragmentResult(size_t index) const
{
    return Builder_->CreateInBoundsGEP(
        TClosureTypeBuilder::Get(
            Builder_->getContext(),
            ExpressionFragments.Functions.size()),
        ExpressionClosurePtr,
        {
            Builder_->getInt32(0),
            Builder_->getInt32(TClosureTypeBuilder::Fields::FragmentResults),
            Builder_->getInt32(ExpressionFragments.Items[index].Index)
        },
        Twine("fragment#") + Twine(index));
}

Value* TCGExprContext::GetFragmentFlag(size_t index) const
{
    return Builder_->CreateInBoundsGEP(
        TClosureTypeBuilder::Get(
            Builder_->getContext(),
            ExpressionFragments.Functions.size()),
        ExpressionClosurePtr,
        {
            Builder_->getInt32(0),
            Builder_->getInt32(TClosureTypeBuilder::Fields::FragmentResults),
            Builder_->getInt32(ExpressionFragments.Items[index].Index),
            Builder_->getInt32(TValueTypeBuilder::Type)
        },
        Twine("flag#") + Twine(index));
}

TCGExprContext TCGExprContext::Make(
    const TCGBaseContext& builder,
    const TCodegenFragmentInfos& fragmentInfos,
    Value* expressionClosurePtr,
    Value* literals,
    Value* rowValues)
{
    Value* opaqueValuesPtr = builder->CreateStructGEP(
        TClosureTypeBuilder::Get(builder->getContext(), fragmentInfos.Functions.size()),
        expressionClosurePtr,
        TClosureTypeBuilder::Fields::OpaqueValues);

    Value* opaqueValues = builder->CreateLoad(
        TClosureTypeBuilder::TOpaqueValues::Get(builder->getContext()),
        opaqueValuesPtr,
        "opaqueValues");

    Value* bufferPtr = builder->CreateStructGEP(
        TClosureTypeBuilder::Get(builder->getContext(), fragmentInfos.Functions.size()),
        expressionClosurePtr,
        TClosureTypeBuilder::Fields::Buffer);

    Value* buffer = builder->CreateLoad(
        TClosureTypeBuilder::TBuffer::Get(builder->getContext()),
        bufferPtr,
        "buffer");

    return TCGExprContext(
        TCGOpaqueValuesContext(builder, literals, opaqueValues),
        TCGExprData(
            fragmentInfos,
            buffer,
            rowValues,
            expressionClosurePtr));
}

TCGExprContext TCGExprContext::Make(
    const TCGOpaqueValuesContext& builder,
    const TCodegenFragmentInfos& fragmentInfos,
    Value* rowValues,
    Value* buffer,
    Value* expressionClosurePtr)
{
    if (!expressionClosurePtr) {
        expressionClosurePtr = builder->CreateAlloca(
            TClosureTypeBuilder::Get(builder->getContext(), fragmentInfos.Functions.size()),
            nullptr,
            "expressionClosurePtr");
    }

    builder->CreateStore(
        builder.GetOpaqueValues(),
        builder->CreateConstInBoundsGEP2_32(
            TClosureTypeBuilder::Get(builder->getContext(), fragmentInfos.Functions.size()),
            expressionClosurePtr,
            0,
            TClosureTypeBuilder::Fields::OpaqueValues));

    builder->CreateStore(
        buffer,
        builder->CreateConstInBoundsGEP2_32(
            TClosureTypeBuilder::Get(builder->getContext(), fragmentInfos.Functions.size()),
            expressionClosurePtr,
            0,
            TClosureTypeBuilder::Fields::Buffer));

    builder->CreateMemSet(
        builder->CreatePointerCast(
            builder->CreateConstInBoundsGEP2_32(
                TClosureTypeBuilder::Get(builder->getContext(), fragmentInfos.Functions.size()),
                expressionClosurePtr,
                0,
                TClosureTypeBuilder::Fields::FragmentResults),
            builder->getInt8PtrTy()),
        builder->getInt8(static_cast<int>(EValueType::TheBottom)),
        sizeof(TValue) * fragmentInfos.Functions.size(),
        llvm::Align(8));

    return TCGExprContext(
        builder,
        TCGExprData(
            fragmentInfos,
            buffer,
            rowValues,
            expressionClosurePtr));
}

Value* TCGExprContext::GetExpressionClosurePtr()
{
    return ExpressionClosurePtr;
}

////////////////////////////////////////////////////////////////////////////////

TCodegenConsumer& TCGOperatorContext::operator[] (size_t index) const
{
    if (!(*Consumers_)[index]) {
        (*Consumers_)[index] = std::make_shared<TCodegenConsumer>();
    }
    return *(*Consumers_)[index];
}

////////////////////////////////////////////////////////////////////////////////

TCGValue MakePhi(
    const TCGIRBuilderPtr& builder,
    BasicBlock* thenBB,
    BasicBlock* elseBB,
    TCGValue thenValue,
    TCGValue elseValue,
    Twine name)
{
    BasicBlock* endBB = builder->GetInsertBlock();

    YT_VERIFY(thenValue.GetStaticType() == elseValue.GetStaticType());
    EValueType type = thenValue.GetStaticType();

    builder->SetInsertPoint(thenBB);
    Value* thenNull = thenValue.IsNull();
    Value* thenData = thenValue.GetData();

    builder->SetInsertPoint(elseBB);
    Value* elseNull = elseValue.IsNull();
    Value* elseData = elseValue.GetData();

    builder->SetInsertPoint(endBB);

    Value* phiNull = [&] () -> Value* {
        if (llvm::Constant* constantThenNull = llvm::dyn_cast<llvm::Constant>(thenNull)) {
            if (llvm::Constant* constantElseNull = llvm::dyn_cast<llvm::Constant>(elseNull)) {
                if (constantThenNull->isNullValue() && constantElseNull->isNullValue()) {
                    return builder->getFalse();
                }
            }
        }

        if (thenNull->getType() != elseNull->getType()) {
            builder->SetInsertPoint(thenBB);
            thenNull = thenValue.GetIsNull(builder);

            builder->SetInsertPoint(elseBB);
            elseNull = elseValue.GetIsNull(builder);

            builder->SetInsertPoint(endBB);
        }

        Type* targetType = thenNull->getType();

        PHINode* phiNull = builder->CreatePHI(targetType, 2, name + ".phiNull");
        phiNull->addIncoming(thenNull, thenBB);
        phiNull->addIncoming(elseNull, elseBB);
        return phiNull;
    }();

    if (thenData->getType() != elseData->getType()) {
        builder->SetInsertPoint(thenBB);
        thenData = thenValue.GetTypedData(builder);

        builder->SetInsertPoint(elseBB);
        elseData = elseValue.GetTypedData(builder);

        builder->SetInsertPoint(endBB);
    }

    YT_VERIFY(thenData->getType() == elseData->getType());

    PHINode* phiData = builder->CreatePHI(thenData->getType(), 2, name + ".phiData");
    phiData->addIncoming(thenData, thenBB);
    phiData->addIncoming(elseData, elseBB);

    PHINode* phiLength = nullptr;
    if (IsStringLikeType(type)) {
        builder->SetInsertPoint(thenBB);
        Value* thenLength = thenValue.GetLength();
        builder->SetInsertPoint(elseBB);
        Value* elseLength = elseValue.GetLength();

        builder->SetInsertPoint(endBB);

        YT_VERIFY(thenLength->getType() == elseLength->getType());

        phiLength = builder->CreatePHI(thenLength->getType(), 2, name + ".phiLength");
        phiLength->addIncoming(thenLength, thenBB);
        phiLength->addIncoming(elseLength, elseBB);
    }

    return TCGValue::Create(builder, phiNull, phiLength, phiData, type, name);
}

Value* MakePhi(
    const TCGIRBuilderPtr& builder,
    BasicBlock* thenBB,
    BasicBlock* elseBB,
    Value* thenValue,
    Value* elseValue,
    Twine name)
{
    PHINode* phiValue = builder->CreatePHI(thenValue->getType(), 2, name + ".phiValue");
    phiValue->addIncoming(thenValue, thenBB);
    phiValue->addIncoming(elseValue, elseBB);
    return phiValue;
}

////////////////////////////////////////////////////////////////////////////////

llvm::Attribute BuildUnwindTableAttribute(llvm::LLVMContext& context)
{
    auto builder = llvm::AttrBuilder(context);
    builder.addUWTableAttr(llvm::UWTableKind::Default);
    return builder.getAttribute(llvm::Attribute::AttrKind::UWTable);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient
