/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"

#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/represent_as.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe::vm {
using ColumnOpType = value::ColumnOpType;

static constexpr auto existsOpType = ColumnOpType{ColumnOpType::kOutputNonNothingOnExpectedInput,
                                                  value::TypeTags::Nothing,
                                                  value::TypeTags::Boolean,
                                                  ColumnOpType::ReturnBoolOnMissing{}};

static const auto existsOp =
    value::makeColumnOp<existsOpType>([](value::TypeTags tag, value::Value val) {
        return std::pair(value::TypeTags::Boolean,
                         value::bitcastFrom<bool>(tag != value::TypeTags::Nothing));
    });

/*
 * Given a ValueBlock as input, returns a ValueBlock of true/false values indicating whether
 * each value in the input was non-Nothing (true) or Nothing (false).
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockExists(ArityType arity) {
    invariant(arity == 1);
    auto [inputOwned, inputTag, inputVal] = getFromStack(0);

    invariant(inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto out = valueBlockIn->map(existsOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

namespace {

bool allBools(const value::TypeTags* tag, size_t sz) {
    for (size_t i = 0; i < sz; ++i) {
        if (tag[i] != value::TypeTags::Boolean) {
            return false;
        }
    }
    return true;
}

struct FillEmptyFunctor {
    FillEmptyFunctor(value::TypeTags fillTag, value::Value fillVal)
        : _fillTag(fillTag), _fillVal(fillVal) {}

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        if (tag == value::TypeTags::Nothing) {
            return value::copyValue(_fillTag, _fillVal);
        }
        return value::copyValue(tag, val);
    }

    value::TypeTags _fillTag;
    value::Value _fillVal;
};

// Currently have an invariant that prevents the fill value being Nothing, need to change this flag
// if that invariant gets removed.
static constexpr auto fillEmptyOpType = ColumnOpType{ColumnOpType::kOutputNonNothingOnMissingInput,
                                                     value::TypeTags::Nothing,
                                                     value::TypeTags::Nothing,
                                                     ColumnOpType::ReturnNonNothingOnMissing{}};

static const auto fillEmptyOp = value::makeColumnOpWithParams<fillEmptyOpType, FillEmptyFunctor>();
}  // namespace

/**
 * Implementation of the valueBlockFillEmpty builtin. This instruction takes a block and an
 * SBE value, and produces a new block where all missing values in the block have been replaced
 * with the SBE value.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockFillEmpty(
    ArityType arity) {
    invariant(arity == 2);
    auto [fillOwned, fillTag, fillVal] = getFromStack(1);
    if (fillTag == value::TypeTags::Nothing) {
        return moveFromStack(0);
    }

    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    invariant(blockTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(blockVal);
    auto out = valueBlockIn->map(fillEmptyOp.bindParams(fillTag, fillVal));

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

/**
 * Implementation of the valueBlockFillEmptyBlock builtin. This instruction takes two blocks of the
 * same size, and produces a new block where all missing values in the first block have been
 * replaced with the correpsonding value in the second block.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockFillEmptyBlock(
    ArityType arity) {
    invariant(arity == 2);
    auto [fillOwned, fillTag, fillVal] = getFromStack(1);
    if (fillTag == value::TypeTags::Nothing) {
        return moveFromStack(0);
    }
    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    tassert(8141618,
            "Arguments of valueBlockFillEmptyBlock must be block of values",
            fillTag == value::TypeTags::valueBlock && blockTag == value::TypeTags::valueBlock);

    auto* fillBlockIn = value::bitcastTo<value::ValueBlock*>(fillVal);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(blockVal);

    auto extractedFill = fillBlockIn->extract();
    auto extractedValue = valueBlockIn->extract();
    tassert(8141601,
            "Fill value and block have a different number of items",
            extractedFill.count == extractedValue.count);

    std::vector<value::Value> valueOut(extractedValue.count);
    std::vector<value::TypeTags> tagOut(extractedValue.count, value::TypeTags::Nothing);

    for (size_t i = 0; i < extractedValue.count; ++i) {
        if (extractedValue.tags[i] == value::TypeTags::Nothing) {
            std::tie(tagOut[i], valueOut[i]) =
                value::copyValue(extractedFill.tags[i], extractedFill.vals[i]);
        } else {
            std::tie(tagOut[i], valueOut[i]) =
                value::copyValue(extractedValue.tags[i], extractedValue.vals[i]);
        }
    }
    auto res = std::make_unique<value::HeterogeneousBlock>(std::move(tagOut), std::move(valueOut));

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

template <bool less>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::valueBlockMinMaxImpl(
    value::ValueBlock* inputBlock, value::ValueBlock* bitsetBlock) {
    auto block = inputBlock->extract();
    auto bitset = bitsetBlock->extract();

    ValueCompare<less> comp{nullptr /* collator */};

    tassert(8137400, "Expected block and bitset to be the same size", block.count == bitset.count);
    tassert(8137401, "Expected bitset to be all bools", allBools(bitset.tags, bitset.count));

    value::TypeTags accTag = value::TypeTags::Nothing;
    value::Value accVal = 0;
    for (size_t i = 0; i < block.count; ++i) {
        if (value::bitcastTo<bool>(bitset[i].second) && accTag == value::TypeTags::Nothing &&
            block.tags[i] != value::TypeTags::Nothing) {
            accTag = block.tags[i];
            accVal = block.vals[i];
        } else if (value::bitcastTo<bool>(bitset[i].second) &&
                   block.tags[i] != value::TypeTags::Nothing) {
            if (comp({block.tags[i], block.vals[i]}, {accTag, accVal})) {
                accTag = block.tags[i], accVal = block.vals[i];
            }
        }
    }

    auto [retTag, retVal] = value::copyValue(accTag, accVal);
    return {true, retTag, retVal};
}

/*
 * Given a ValueBlock and bitset as input, returns a tag, value pair that contains the minimum value
 * in the block based on compareValue. Values whose corresponding bit is set to false get ignored.
 * This function will return a non-Nothing value if the block contains any non-Nothing values.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockMin(ArityType arity) {
    invariant(arity == 2);

    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    invariant(inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(0);
    invariant(bitsetTag == value::TypeTags::valueBlock);
    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);

    return valueBlockMinMaxImpl<true /* less */>(valueBlockIn, bitsetBlock);
}

/*
 * Given a ValueBlock and bitset as input, returns a tag, value pair that contains the maximum value
 * in the block based on compareValue. Values whose corresponding bit is set to false get ignored.
 * This function will return a non-Nothing value if the block contains any non-Nothing values.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockMax(ArityType arity) {
    invariant(arity == 2);

    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    invariant(inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(0);
    invariant(bitsetTag == value::TypeTags::valueBlock);
    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);

    return valueBlockMinMaxImpl<false /* less */>(valueBlockIn, bitsetBlock);
}

/*
 * TODO: Comment.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCount(ArityType arity) {
    MONGO_UNREACHABLE;
}

namespace {
template <class Cmp>
FastTuple<bool, value::TypeTags, value::Value> blockCompareGeneric(value::ValueBlock* blockView,
                                                                   value::TypeTags rhsTag,
                                                                   value::Value rhsVal) {

    static constexpr auto cmpOpType = ColumnOpType{ColumnOpType::kOutputNothingOnMissingInput,
                                                   value::TypeTags::Nothing,
                                                   value::TypeTags::Nothing,
                                                   ColumnOpType::ReturnNothingOnMissing{}};

    const auto cmpOp = value::makeColumnOp<cmpOpType>([&](value::TypeTags tag, value::Value val) {
        return genericCompare<Cmp>(tag, val, rhsTag, rhsVal);
    });

    auto res = blockView->map(cmpOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

}  // namespace

template <class Cmp>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCmpScalar(
    ArityType arity) {
    invariant(arity == 2);
    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    invariant(blockTag == value::TypeTags::valueBlock);
    auto [valueOwned, valueTag, valueVal] = getFromStack(1);

    auto blockView = value::getValueBlock(blockVal);
    return blockCompareGeneric<Cmp>(blockView, valueTag, valueVal);
}

/*
 * Comparison against scalar functions.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockGtScalar(
    ArityType arity) {
    return builtinValueBlockCmpScalar<std::greater<>>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockGteScalar(
    ArityType arity) {
    return builtinValueBlockCmpScalar<std::greater_equal<>>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockEqScalar(
    ArityType arity) {
    return builtinValueBlockCmpScalar<std::equal_to<>>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockNeqScalar(
    ArityType arity) {
    auto [blockOwned, blockTag, blockVal] = builtinValueBlockCmpScalar<std::equal_to<>>(arity);
    value::ValueGuard guard(blockTag, blockVal);

    // For neq we apply equal_to and then use genericNot() to negate it, just like the scalar
    // variation in the VM.
    static constexpr auto notOpType = ColumnOpType{ColumnOpType::kOutputNothingOnMissingInput,
                                                   value::TypeTags::Nothing,
                                                   value::TypeTags::Nothing,
                                                   ColumnOpType::ReturnNothingOnMissing{}};

    const auto notOp = value::makeColumnOp<notOpType>(
        [&](value::TypeTags tag, value::Value val) { return genericNot(tag, val); });

    invariant(blockTag == value::TypeTags::valueBlock);

    auto res = value::getValueBlock(blockVal)->map(notOp);
    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLtScalar(
    ArityType arity) {
    return builtinValueBlockCmpScalar<std::less<>>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLteScalar(
    ArityType arity) {
    return builtinValueBlockCmpScalar<std::less_equal<>>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCmp3wScalar(
    ArityType arity) {
    invariant(arity == 2);
    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    invariant(blockTag == value::TypeTags::valueBlock);
    auto value = getFromStack(1);

    auto blockView = value::getValueBlock(blockVal);

    static constexpr auto cmpOpType = ColumnOpType{ColumnOpType::kOutputNothingOnMissingInput,
                                                   value::TypeTags::Nothing,
                                                   value::TypeTags::Nothing,
                                                   ColumnOpType::ReturnNothingOnMissing{}};

    const auto cmpOp = value::makeColumnOp<cmpOpType>([&](value::TypeTags tag, value::Value val) {
        return compare3way(tag, val, value.b, value.c);
    });

    auto res = blockView->map(cmpOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

/*
 * Given two blocks and a mask of equal size, return a new block having the values from the first
 * argument when the matching entry in the mask is True, and the values from the second argument
 * when the matching entry in the mask is False.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCombine(ArityType arity) {
    invariant(arity == 3);

    auto [bitmapOwned, bitmapTag, bitmapVal] = getFromStack(2);
    tassert(8141609,
            "valueBlockCombine expects a block of boolean values as mask",
            bitmapTag == value::TypeTags::valueBlock);
    auto* bitmap = value::getValueBlock(bitmapVal);
    auto bitmapExtracted = bitmap->extract();
    tassert(8141610,
            "valueBlockCombine expects a block of boolean values as mask",
            allBools(bitmapExtracted.tags, bitmapExtracted.count));

    size_t numTrue = 0;
    for (size_t i = 0; i < bitmapExtracted.count; i++) {
        numTrue += value::bitcastTo<bool>(bitmapExtracted.vals[i]);
    }
    auto promoteArgAsResult =
        [&](size_t stackPos) -> FastTuple<bool, value::TypeTags, value::Value> {
        auto [owned, tag, val] = moveFromStack(stackPos);
        tassert(8141611,
                "valueBlockCombine expects a block as argument",
                tag == value::TypeTags::valueBlock);
        auto* rhsBlock = value::getValueBlock(val);
        auto count = rhsBlock->tryCount();
        if (!count.has_value()) {
            count = rhsBlock->extract().count;
        }
        tassert(8141612,
                "valueBlockCombine expects the arguments to have the same size",
                *count == bitmapExtracted.count);
        return {owned, tag, val};
    };
    if (numTrue == 0) {
        return promoteArgAsResult(1);
    } else if (numTrue == bitmapExtracted.count) {
        return promoteArgAsResult(0);
    }

    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
    tassert(8141615,
            "valueBlockCombine expects a block as first argument",
            lhsTag == value::TypeTags::valueBlock);
    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(1);
    tassert(8141616,
            "valueBlockCombine expects a block as second argument",
            rhsTag == value::TypeTags::valueBlock);
    auto* lhsBlock = value::getValueBlock(lhsVal);
    auto* rhsBlock = value::getValueBlock(rhsVal);

    auto lhsExtracted = lhsBlock->extract();
    auto rhsExtracted = rhsBlock->extract();
    tassert(8141617,
            "valueBlockCombine expects the arguments to have the same size",
            lhsExtracted.count == rhsExtracted.count &&
                lhsExtracted.count == bitmapExtracted.count);

    std::vector<value::Value> valueOut(bitmapExtracted.count);
    std::vector<value::TypeTags> tagOut(bitmapExtracted.count, value::TypeTags::Nothing);
    for (size_t i = 0; i < bitmapExtracted.count; i++) {
        if (value::bitcastTo<bool>(bitmapExtracted.vals[i])) {
            std::tie(tagOut[i], valueOut[i]) =
                value::copyValue(lhsExtracted.tags[i], lhsExtracted.vals[i]);
        } else {
            std::tie(tagOut[i], valueOut[i]) =
                value::copyValue(rhsExtracted.tags[i], rhsExtracted.vals[i]);
        }
    }
    auto blockOut =
        std::make_unique<value::HeterogeneousBlock>(std::move(tagOut), std::move(valueOut));
    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

static constexpr auto invokeLambdaOpType = ColumnOpType{ColumnOpType::kNoFlags,
                                                        value::TypeTags::Nothing,
                                                        value::TypeTags::Nothing,
                                                        ColumnOpType::OnMissingInput{}};

static const auto invokeLambdaOp =
    value::makeColumnOpWithParams<invokeLambdaOpType, ByteCode::InvokeLambdaFunctor>();

/**
 * Implementation of the valueBlockApplyLambda instruction. This instruction takes a mask, a block
 * and an SBE lambda f(), and produces a new block with the result of f() applied to each element of
 * the input for which the mask has, in the same position, a 'true' value.
 * A mask value of Nothing is equivalent to a mask full of 'true' values.
 */
void ByteCode::valueBlockApplyLambda(const CodeFragment* code) {
    auto [lamOwn, lamTag, lamVal] = moveFromStack(0);
    popAndReleaseStack();
    value::ValueGuard lamGuard(lamOwn, lamTag, lamVal);

    auto [blockOwn, blockTag, blockVal] = moveFromStack(0);
    popAndReleaseStack();
    value::ValueGuard blockGuard(blockOwn, blockTag, blockVal);

    auto [maskOwn, maskTag, maskVal] = moveFromStack(0);
    popAndReleaseStack();
    value::ValueGuard maskGuard(maskOwn, maskTag, maskVal);

    if (lamTag != value::TypeTags::LocalLambda) {
        pushStack(false, value::TypeTags::Nothing, 0);
        return;
    }

    if (blockTag != value::TypeTags::valueBlock) {
        pushStack(false, value::TypeTags::Nothing, 0);
        return;
    }

    const auto lamPos = value::bitcastTo<int64_t>(lamVal);
    auto* block = value::getValueBlock(blockVal);

    std::unique_ptr<value::ValueBlock> outBlock;
    if (maskTag == value::TypeTags::valueBlock) {
        // We have a valid mask, loop only over the enabled indexes.
        auto* mask = value::getValueBlock(maskVal);
        auto extractedMask = mask->extract();
        auto extracted = block->extract();
        tassert(8123000,
                "Mask and block have a different number of items",
                extracted.count == extractedMask.count);
        tassert(8123001,
                "Expected mask to be all bool values",
                allBools(extractedMask.tags, extractedMask.count));

        // Pre-fill with Nothing, and overwrite only the allowed indexes.
        std::vector<value::Value> valueOut(extracted.count);
        std::vector<value::TypeTags> tagOut(extracted.count, value::TypeTags::Nothing);

        ByteCode::InvokeLambdaFunctor invoker(*this, code, lamPos);
        for (size_t i = 0; i < extracted.count; ++i) {
            if (value::bitcastTo<bool>(extractedMask.vals[i])) {
                std::tie(tagOut[i], valueOut[i]) = invoker(extracted.tags[i], extracted.vals[i]);
            }
        }
        outBlock =
            std::make_unique<value::HeterogeneousBlock>(std::move(tagOut), std::move(valueOut));
    } else {
        outBlock = block->map(invokeLambdaOp.bindParams(*this, code, lamPos));
    }

    pushStack(true,
              value::TypeTags::valueBlock,
              value::bitcastFrom<value::ValueBlock*>(outBlock.release()));
}

template <class Op>
std::unique_ptr<value::ValueBlock> applyBoolBinOp(value::ValueBlock* leftBlock,
                                                  value::ValueBlock* rightBlock,
                                                  Op op = {}) {
    auto left = leftBlock->extract();
    auto right = rightBlock->extract();

    tassert(7953531, "Mismatch on size", left.count == right.count);
    // Check that both contain all booleans.
    bool allBool = allBools(left.tags, left.count) && allBools(right.tags, right.count);
    tassert(7953532, "Expected all bool inputs", allBool);

    std::vector<value::Value> boolOut(left.count);
    std::vector<value::TypeTags> tagOut(left.count, value::TypeTags::Boolean);

    for (size_t i = 0; i < left.count; ++i) {
        const auto leftBool = value::bitcastTo<bool>(left.vals[i]);
        const auto rightBool = value::bitcastTo<bool>(right.vals[i]);
        boolOut[i] = value::bitcastFrom<bool>(op(leftBool, rightBool));
    }

    return std::make_unique<value::HeterogeneousBlock>(std::move(tagOut), std::move(boolOut));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLogicalAnd(
    ArityType arity) {
    invariant(arity == 2);

    auto [leftOwned, leftTag, leftVal] = getFromStack(1);
    invariant(leftTag == value::TypeTags::valueBlock);
    auto* leftValueBlock = value::bitcastTo<value::ValueBlock*>(leftVal);

    auto [rightOwned, rightTag, rightVal] = getFromStack(0);
    invariant(rightTag == value::TypeTags::valueBlock);
    auto* rightValueBlock = value::bitcastTo<value::ValueBlock*>(rightVal);

    auto blockOut = applyBoolBinOp<std::logical_and<>>(leftValueBlock, rightValueBlock);
    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLogicalOr(
    ArityType arity) {
    invariant(arity == 2);

    auto [leftOwned, leftTag, leftVal] = getFromStack(1);
    invariant(leftTag == value::TypeTags::valueBlock);
    auto* leftValueBlock = value::bitcastTo<value::ValueBlock*>(leftVal);

    auto [rightOwned, rightTag, rightVal] = getFromStack(0);
    invariant(rightTag == value::TypeTags::valueBlock);
    auto* rightValueBlock = value::bitcastTo<value::ValueBlock*>(rightVal);

    auto blockOut = applyBoolBinOp<std::logical_or<>>(leftValueBlock, rightValueBlock);
    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockNewFill(ArityType arity) {
    invariant(arity == 2);

    auto [rightOwned, rightTag, rightVal] = getFromStack(1);
    auto [countOwned, countTag, countVal] =
        value::genericNumConvert(rightTag, rightVal, value::TypeTags::NumberInt32);
    tassert(8141602,
            "valueBlockNewFill expects an integer in the size argument",
            countTag == value::TypeTags::NumberInt32);

    // Take ownership of the value, we are transferring it to the block.
    auto [leftOwned, leftTag, leftVal] = moveFromStack(0);
    if (!leftOwned) {
        std::tie(leftTag, leftVal) = value::copyValue(leftTag, leftVal);
    }
    auto blockOut =
        std::make_unique<value::MonoBlock>(value::bitcastTo<int32_t>(countVal), leftTag, leftVal);
    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockSize(ArityType arity) {
    invariant(arity == 1);

    auto [_, blockTag, blockVal] = getFromStack(0);
    tassert(8141603,
            "valueBlockSize expects a block as argument",
            blockTag == value::TypeTags::valueBlock);
    auto* block = value::getValueBlock(blockVal);
    auto count = block->tryCount();
    if (!count.has_value()) {
        count = block->extract().count;
    }
    tassert(8141604, "block exceeds maximum length", mongo::detail::inRange<int32_t>(*count));

    return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(*count)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockNone(ArityType arity) {
    invariant(arity == 2);

    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    tassert(8141605,
            "valueBlockNone expects a block as first argument",
            blockTag == value::TypeTags::valueBlock);
    auto [searchOwned, searchTag, searchVal] = getFromStack(1);

    auto* block = value::getValueBlock(blockVal);
    auto extracted = block->extract();

    for (size_t i = 0; i < extracted.count; i++) {
        auto [cmpTag, cmpVal] =
            sbe::value::compareValue(extracted.tags[i], extracted.vals[i], searchTag, searchVal);
        if (cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0) {
            return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false)};
        }
    }
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(true)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLogicalNot(
    ArityType arity) {
    invariant(arity == 1);

    auto [bitmapOwned, bitmapTag, bitmapVal] = getFromStack(0);
    tassert(8141607,
            "valueBlockLogicalNot expects a block of boolean values as argument",
            bitmapTag == value::TypeTags::valueBlock);

    auto bitmapView = value::getValueBlock(bitmapVal);

    static constexpr auto cmpOpType = ColumnOpType{ColumnOpType::kOutputNothingOnMissingInput,
                                                   value::TypeTags::Nothing,
                                                   value::TypeTags::Nothing,
                                                   ColumnOpType::ReturnNothingOnMissing{}};

    const auto cmpOp = value::makeColumnOp<cmpOpType>([&](value::TypeTags tag, value::Value val) {
        tassert(8141608,
                "valueBlockLogicalNot expects a block of boolean values as argument",
                tag == value::TypeTags::Boolean);
        return std::make_pair(tag, !value::bitcastTo<bool>(val));
    });

    auto res = bitmapView->map(cmpOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCellFoldValues_F(ArityType arity) {
    auto [valBlockOwned, valBlockTag, valBlockVal] = getFromStack(0);
    invariant(valBlockTag == value::TypeTags::valueBlock);
    auto* valueBlock = value::bitcastTo<value::ValueBlock*>(valBlockVal);

    auto [cellOwned, cellTag, cellVal] = getFromStack(1);
    invariant(cellTag == value::TypeTags::cellBlock);
    auto* cellBlock = value::bitcastTo<value::CellBlock*>(cellVal);

    auto valsExtracted = valueBlock->extract();
    tassert(7953533, "Expected all bool inputs", allBools(valsExtracted.tags, valsExtracted.count));

    const auto& positionInfo = cellBlock->filterPositionInfo();
    if (positionInfo.empty()) {
        // Return the input unchanged.
        return moveFromStack(0);
    }

    tassert(7953534,
            "Expected position info count to be same as value size",
            valsExtracted.count == positionInfo.size());
    tassert(7953535, "Unsupported empty block", valsExtracted.count > 0);
    tassert(7953536, "First position info element should always be true", positionInfo[0]);

    // Note: if this code ends up being a bottleneck, we can make some changes. foldCounts()
    // can be initialized based on the number of 1 bits in filterPosInfo. We can also try to
    // make 'folded' and 'foldCounts' use one buffer, rather than two.

    // Represents number of true values in each run.
    std::vector<int> foldCounts(valsExtracted.count, 0);
    int runsSeen = -1;
    for (size_t i = 0; i < valsExtracted.count; ++i) {
        dassert(positionInfo[i] == 1 || positionInfo[i] == 0);
        runsSeen += positionInfo[i];
        foldCounts[runsSeen] += static_cast<int>(value::bitcastTo<bool>(valsExtracted[i].second));
    }

    // The last run is implicitly ended.
    ++runsSeen;

    std::vector<value::Value> folded(runsSeen);
    for (size_t i = 0; i < folded.size(); ++i) {
        folded[i] = value::bitcastFrom<bool>(static_cast<bool>(foldCounts[i]));
    }

    auto blockOut = std::make_unique<value::HeterogeneousBlock>(
        std::vector<value::TypeTags>(folded.size(), value::TypeTags::Boolean), std::move(folded));

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCellFoldValues_P(ArityType arity) {
    MONGO_UNREACHABLE;
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCellBlockGetFlatValuesBlock(
    ArityType arity) {
    invariant(arity == 1);
    auto [cellOwn, cellTag, cellVal] = getFromStack(0);

    if (cellTag != value::TypeTags::cellBlock) {
        return {false, value::TypeTags::Nothing, 0};
    }
    tassert(7946600, "Cannot process temporary cell values", !cellOwn);

    auto* cell = value::getCellBlock(cellVal);

    return {false,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(&cell->getValueBlock())};
}

}  // namespace mongo::sbe::vm
