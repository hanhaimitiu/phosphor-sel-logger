/*
// Copyright (c) 2017 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
/* // 版权所有 (c) 2017 2018 Intel Corporation
 * //
 * // 本文件受 Apache License, Version 2.0（以下简称 "License"）许可；
 * // 除非符合该 License，否则你不得使用本文件。
 * // 你可以在以下地址获取该 License 的副本：
 * //
 * //      http://www.apache.org/licenses/LICENSE-2.0
 * //
 * // 除非适用法律要求或书面同意，否则软件
 * // 均按 "AS IS"（原样）基础分发，
 * // 不附带任何明示或暗示的保证或条件。
 * // 有关该 License 下的具体权限和限制，
 * // 请参阅该 License。 */

#pragma once
#include <algorithm>
#include <cmath>
#include <iostream>

namespace ipmi
{
/** @struct VariantToDoubleVisitor
 *  @brief Visitor to convert variants to doubles
 *  @details Performs a static cast on the underlying type
 */
/* @struct VariantToDoubleVisitor
 *  @brief 将 variant 转换为 double 的访问器（Visitor）
 *  @details 对底层类型执行 static cast（静态转换） */
struct VariantToDoubleVisitor
{
    template <typename T>
    double operator()(const T& t) const
    {
        static_assert(std::is_arithmetic_v<T>,
                      "Cannot translate type to double");
        return static_cast<double>(t);
    }
};

static constexpr int16_t maxInt10 = 0x1FF;
static constexpr int16_t minInt10 = -0x200;
static constexpr int8_t maxInt4 = 7;
static constexpr int8_t minInt4 = -8;

// Helper function to avoid repeated complicated expression
// TODO(): Refactor to add a proper sensorutils.cpp file,
// instead of putting everything in this header as it is now,
// so that helper functions can be correctly hidden from callers.
// 辅助函数，避免重复复杂的表达式
// TODO(): 重构，新增一个合适的 sensorutils.cpp 文件，
// 而不是像现在这样把所有东西都放在这个头文件里，
// 这样辅助函数才能真正对调用方隐藏。
static inline bool baseInRange(double base)
{
    auto min10 = static_cast<double>(minInt10);
    auto max10 = static_cast<double>(maxInt10);

    return ((base >= min10) && (base <= max10));
}

// Helper function for internal use by getSensorAttributes()
// Ensures floating-point "base" is within bounds,
// and adjusts integer exponent "expShift" accordingly.
// To minimize data loss when later truncating to integer,
// the floating-point "base" will be as large as possible,
// but still within the bounds (minInt10,maxInt10).
// The bounds of "expShift" are (minInt4,maxInt4).
// Consider this equation: n = base * (10.0 ** expShift)
// This function will try to maximize "base",
// adjusting "expShift" to keep the value "n" unchanged,
// while keeping base and expShift within bounds.
// Returns true if successful, modifies values in-place
// 供 getSensorAttributes() 内部使用的辅助函数
// 确保浮点数 "base" 在边界范围内，
// 并相应地调整整数指数 "expShift"。
// 为尽量减少后续截断为整数时的数据损失，
// 浮点数 "base" 会尽可能取大，
// 但仍保持在边界 (minInt10,maxInt10) 之内。
// "expShift" 的边界是 (minInt4,maxInt4)。
// 考虑这个等式：n = base * (10.0 ** expShift)
// 本函数会尽量最大化 "base"，
// 同时调整 "expShift" 以保持 "n" 的值不变，
// 并使 base 和 expShift 都保持在边界内。
// 成功时返回 true，并原地修改传入的值。
static inline bool scaleFloatExp(double& base, int8_t& expShift)
{
    // Comparing with zero should be OK, zero is special in floating-point
    // If base is exactly zero, no adjustment of the exponent is necessary
    // 与零比较应该没问题，零在浮点数中是特殊的
    // 如果 base 恰好为零，就无需调整指数
    if (base == 0.0)
    {
        return true;
    }

    // As long as base value is within allowed range, expand precision
    // This will help to avoid loss when later rounding to integer
    // 只要 base 值在允许的范围内，就扩展精度
    // 这有助于避免后续舍入为整数时的损失
    while (baseInRange(base))
    {
        if (expShift <= minInt4)
        {
            // Already at the minimum expShift, can not decrement it more
            // expShift 已到最小值，不能再减小
            break;
        }

        // Multiply by 10, but shift decimal point to the left, no net change
        // 乘以 10，但将小数点左移，整体值不变
        base *= 10.0;
        --expShift;
    }

    // As long as base value is *not* within range, shrink precision
    // This will pull base value closer to zero, thus within range
    // 只要 base 值*不*在范围内，就缩小精度
    // 这会把 base 值拉近零，从而进入范围
    while (!(baseInRange(base)))
    {
        if (expShift >= maxInt4)
        {
            // Already at the maximum expShift, can not increment it more
            // expShift 已到最大值，不能再增大
            break;
        }

        // Divide by 10, but shift decimal point to the right, no net change
        // 除以 10，但将小数点右移，整体值不变
        base /= 10.0;
        ++expShift;
    }

    // If the above loop was not able to pull it back within range,
    // the base value is beyond what expShift can represent, return false.
    // 如果上面的循环没能把它拉回范围内，
    // 说明 base 值超出了 expShift 能表示的范围，返回 false。
    return baseInRange(base);
}

// Helper function for internal use by getSensorAttributes()
// Ensures integer "ibase" is no larger than necessary,
// by normalizing it so that the decimal point shift is in the exponent,
// whenever possible.
// This provides more consistent results,
// as many equivalent solutions are collapsed into one consistent solution.
// If integer "ibase" is a clean multiple of 10,
// divide it by 10 (this is lossless), so it is closer to zero.
// Also modify floating-point "dbase" at the same time,
// as both integer and floating-point base share the same expShift.
// Example: (ibase=300, expShift=2) becomes (ibase=3, expShift=4)
// because the underlying value is the same: 200*(10**2) == 2*(10**4)
// Always successful, modifies values in-place
// 供 getSensorAttributes() 内部使用的辅助函数
// 确保整数 "ibase" 不会大于必要值，
// 方法是将其规范化，把小数点移位放到指数中，
// 在可能的情况下。
// 这样能得到更一致的结果，
// 因为许多等价解会被归并为同一个一致的解。
// 如果整数 "ibase" 是 10 的整数倍，
// 就除以 10（这是无损的），使其更接近零。
// 同时也要修改浮点数 "dbase"，
// 因为整数和浮点数的 base 共享同一个 expShift。
// 例如：(ibase=300, expShift=2) 变成 (ibase=3, expShift=4)
// 因为底层值相同：200*(10**2) == 2*(10**4)
// 总是成功，原地修改传入的值。
static inline void normalizeIntExp(int16_t& ibase, int8_t& expShift,
                                   double& dbase)
{
    for (;;)
    {
        // If zero, already normalized, ensure exponent also zero
        // 若为零，则已规范化，同时确保指数也为零
        if (ibase == 0)
        {
            expShift = 0;
            break;
        }

        // If not cleanly divisible by 10, already normalized
        // 若不能被 10 整除，则已规范化
        if ((ibase % 10) != 0)
        {
            break;
        }

        // If exponent already at max, already normalized
        // 若指数已达最大值，则已规范化
        if (expShift >= maxInt4)
        {
            break;
        }

        // Bring values closer to zero, correspondingly shift exponent,
        // without changing the underlying number that this all represents,
        // similar to what is done by scaleFloatExp().
        // The floating-point base must be kept in sync with the integer base,
        // as both floating-point and integer share the same exponent.
        // 使各值更接近零，并相应调整指数，
        // 而不改变这一切所表示的底层数值，
        // 与 scaleFloatExp() 所做的类似。
        // 浮点数 base 必须与整数 base 保持同步，
        // 因为浮点数和整数共享同一个指数。
        ibase /= 10;
        dbase /= 10.0;
        ++expShift;
    }
}

// The IPMI equation:
// y = (Mx + (B * 10^(bExp))) * 10^(rExp)
// Section 36.3 of this document:
// https://www.intel.com/content/dam/www/public/us/en/documents/product-briefs/ipmi-second-gen-interface-spec-v2-rev1-1.pdf
//
// The goal is to exactly match the math done by the ipmitool command,
// at the other side of the interface:
// https://github.com/ipmitool/ipmitool/blob/42a023ff0726c80e8cc7d30315b987fe568a981d/lib/ipmi_sdr.c#L360
//
// To use with Wolfram Alpha, make all variables single letters
// bExp becomes E, rExp becomes R
// https://www.wolframalpha.com/input/?i=y%3D%28%28M*x%29%2B%28B*%2810%5EE%29%29%29*%2810%5ER%29
// IPMI 公式：
// y = (Mx + (B * 10^(bExp))) * 10^(rExp)
// 本文档第 36.3 节：
// https://www.intel.com/content/dam/www/public/us/en/documents/product-briefs/ipmi-second-gen-interface-spec-v2-rev1-1.pdf
// 目标是与 ipmitool 命令的计算完全一致，
// 即接口另一端的实现：
// https://github.com/ipmitool/ipmitool/blob/42a023ff0726c80e8cc7d30315b987fe568a981d/lib/ipmi_sdr.c#L360
// 要在 Wolfram Alpha 中使用，把所有变量改成单个字母
// bExp 改为 E，rExp 改为 R
// https://www.wolframalpha.com/input/?i=y%3D%28%28M*x%29%2B%28B*%2810%5EE%29%29%29*%2810%5ER%29
static inline bool getSensorAttributes(
    const double max, const double min, int16_t& mValue, int8_t& rExp,
    int16_t& bValue, int8_t& bExp, bool& bSigned)
{
    if (!(std::isfinite(min)))
    {
        std::cerr << "getSensorAttributes: Min value is unusable\n";
        return false;
    }
    if (!(std::isfinite(max)))
    {
        std::cerr << "getSensorAttributes: Max value is unusable\n";
        return false;
    }

    // Because NAN has already been tested for, this comparison works
    // 由于已经检测过 NAN，这个比较是有效的
    if (max <= min)
    {
        std::cerr << "getSensorAttributes: Max must be greater than min\n";
        return false;
    }

    // Given min and max, we must solve for M, B, bExp, rExp
    // y comes in from D-Bus (the actual sensor reading)
    // x is calculated from y by scaleIPMIValueFromDouble() below
    // If y is min, x should equal = 0 (or -128 if signed)
    // If y is max, x should equal 255 (or 127 if signed)
    // 给定 min 和 max，需要求解 M、B、bExp、rExp
    // y 来自 D-Bus（实际的传感器读数）
    // x 由下面的 scaleIPMIValueFromDouble() 根据 y 计算得出
    // 若 y 为 min，则 x 应等于 0（有符号时为 -128）
    // 若 y 为 max，则 x 应等于 255（有符号时为 127）
    double fullRange = max - min;
    double lowestX;

    rExp = 0;
    bExp = 0;

    // TODO(): The IPMI document is ambiguous, as to whether
    // the resulting byte should be signed or unsigned,
    // essentially leaving it up to the caller.
    // The document just refers to it as "raw reading",
    // or "byte of reading", without giving further details.
    // Previous code set it signed if min was less than zero,
    // so I'm sticking with that, until I learn otherwise.
    // TODO(): IPMI 文档对于结果字节是否
    // 为有符号还是无符号含糊不清，
    // 实际上把它交给调用方决定。
    // 文档只称其为 "raw reading"（原始读数），
    // 或 "byte of reading"（读数字节），未作进一步说明。
    // 之前的代码在 min 小于零时将其设为有符号，
    // 所以在了解到其他情况之前，我暂且沿用这一做法。
    if (min < 0.0)
    {
        // TODO(): It would be worth experimenting with the range (-127,127),
        // instead of the range (-128,127), because this
        // would give good symmetry around zero, and make results look better.
        // Divide by 254 instead of 255, and change -128 to -127 elsewhere.
        // TODO(): 值得试试用范围 (-127,127)
        // 代替 (-128,127)，因为这样
        // 能围绕零很好地对称，使结果更美观。
        // 用 254 代替 255 做除法，并在别处把 -128 改成 -127。
        bSigned = true;
        lowestX = -128.0;
    }
    else
    {
        bSigned = false;
        lowestX = 0.0;
    }

    // Step 1: Set y to (max - min), set x to 255, set B to 0, solve for M
    // This works, regardless of signed or unsigned,
    // because total range is the same.
    // 步骤 1：令 y = (max - min)，令 x = 255，令 B = 0，求解 M
    // 无论有符号还是无符号，这都成立，
    // 因为总范围相同。
    double dM = fullRange / 255.0;

    // Step 2: Constrain M, and set rExp accordingly
    // 步骤 2：约束 M，并相应设置 rExp
    if (!(scaleFloatExp(dM, rExp)))
    {
        std::cerr << "getSensorAttributes: Multiplier range exceeds scale (M="
                  << dM << ", rExp=" << (int)rExp << ")\n";
        return false;
    }

    mValue = static_cast<int16_t>(std::round(dM));

    normalizeIntExp(mValue, rExp, dM);

    // The multiplier can not be zero, for obvious reasons
    // 乘数显然不能为零
    if (mValue == 0)
    {
        std::cerr << "getSensorAttributes: Multiplier range below scale\n";
        return false;
    }

    // Step 3: set y to min, set x to min, keep M and rExp, solve for B
    // If negative, x will be -128 (the most negative possible byte), not 0
    // 步骤 3：令 y 为 min，令 x 为 min，保持 M 和 rExp，求解 B
    // 若为负数，x 将是 -128（字节能表示的最小值），而不是 0

    // Solve the IPMI equation for B, instead of y
    // https://www.wolframalpha.com/input/?i=solve+y%3D%28%28M*x%29%2B%28B*%2810%5EE%29%29%29*%2810%5ER%29+for+B
    // B = 10^(-rExp - bExp) (y - M 10^rExp x)
    // TODO(): Compare with this alternative solution from SageMathCell
    // https://sagecell.sagemath.org/?z=eJyrtC1LLNJQr1TX5KqAMCuATF8I0xfIdIIwnYDMIteKAggPxAIKJMEFkiACxfk5Zaka0ZUKtrYKGhq-CloKFZoK2goaTkCWhqGBgpaWAkilpqYmQgBklmasjoKTJgDAECTH&lang=sage&interacts=eJyLjgUAARUAuQ==
    // 对 IPMI 公式求解 B，而不是 y
    // https://www.wolframalpha.com/input/?i=solve+y%3D%28%28M*x%29%2B%28B*%2810%5EE%29%29%29*%2810%5ER%29+for+B
    // B = 10^(-rExp - bExp) (y - M 10^rExp x)
    // TODO(): 与 SageMathCell 给出的这个替代解法对比
    // https://sagecell.sagemath.org/?z=eJyrtC1LLNJQr1TX5KqAMCuATF8I0xfIdIIwnYDMIteKAggPxAIKJMEFkiACxfk5Zaka0ZUKtrYKGhq-CloKFZoK2goaTkCWhqGBgpaWAkilpqYmQgBklmasjoKTJgDAECTH&lang=sage&interacts=eJyLjgUAARUAuQ==
    double dB = std::pow(10.0, ((-rExp) - bExp)) *
                (min - ((dM * std::pow(10.0, rExp) * lowestX)));

    // Step 4: Constrain B, and set bExp accordingly
    // 步骤 4：约束 B，并相应设置 bExp
    if (!(scaleFloatExp(dB, bExp)))
    {
        std::cerr << "getSensorAttributes: Offset (B=" << dB << ", bExp="
                  << (int)bExp << ") exceeds multiplier scale (M=" << dM
                  << ", rExp=" << (int)rExp << ")\n";
        return false;
    }

    bValue = static_cast<int16_t>(std::round(dB));

    normalizeIntExp(bValue, bExp, dB);

    // Unlike the multiplier, it is perfectly OK for bValue to be zero
    // 与乘数不同，bValue 完全可以为零
    return true;
}

static inline uint8_t scaleIPMIValueFromDouble(
    const double value, const int16_t mValue, const int8_t rExp,
    const int16_t bValue, const int8_t bExp, const bool bSigned)
{
    // Avoid division by zero below
    // 避免下面出现除以零
    if (mValue == 0)
    {
        throw std::out_of_range("Scaling multiplier is uninitialized");
    }

    auto dM = static_cast<double>(mValue);
    auto dB = static_cast<double>(bValue);

    // Solve the IPMI equation for x, instead of y
    // https://www.wolframalpha.com/input/?i=solve+y%3D%28%28M*x%29%2B%28B*%2810%5EE%29%29%29*%2810%5ER%29+for+x
    // x = (10^(-rExp) (y - B 10^(rExp + bExp)))/M and M 10^rExp!=0
    // TODO(): Compare with this alternative solution from SageMathCell
    // https://sagecell.sagemath.org/?z=eJyrtC1LLNJQr1TX5KqAMCuATF8I0xfIdIIwnYDMIteKAggPxAIKJMEFkiACxfk5Zaka0ZUKtrYKGhq-CloKFZoK2goaTkCWhqGBgpaWAkilpqYmQgBklmasDlAlAMB8JP0=&lang=sage&interacts=eJyLjgUAARUAuQ==
    // 对 IPMI 公式求解 x，而不是 y
    // https://www.wolframalpha.com/input/?i=solve+y%3D%28%28M*x%29%2B%28B*%2810%5EE%29%29%29*%2810%5ER%29+for+x
    // x = (10^(-rExp) (y - B 10^(rExp + bExp)))/M，且 M 10^rExp != 0
    // TODO(): 与 SageMathCell 给出的这个替代解法对比
    // https://sagecell.sagemath.org/?z=eJyrtC1LLNJQr1TX5KqAMCuATF8I0xfIdIIwnYDMIteKAggPxAIKJMEFkiACxfk5Zaka0ZUKtrYKGhq-CloKFZoK2goaTkCWhqGBgpaWAkilpqYmQgBklmasDlAlAMB8JP0=&lang=sage&interacts=eJyLjgUAARUAuQ==
    double dX =
        (std::pow(10.0, -rExp) * (value - (dB * std::pow(10.0, rExp + bExp)))) /
        dM;

    auto scaledValue = static_cast<int32_t>(std::round(dX));

    int32_t minClamp;
    int32_t maxClamp;

    // Because of rounding and integer truncation of scaling factors,
    // sometimes the resulting byte is slightly out of range.
    // Still allow this, but clamp the values to range.
    // 由于缩放因子的舍入和整数截断，
    // 有时得到的字节会略微超出范围。
    // 仍允许这种情况，但会把值钳制到范围内。
    if (bSigned)
    {
        minClamp = std::numeric_limits<int8_t>::lowest();
        maxClamp = std::numeric_limits<int8_t>::max();
    }
    else
    {
        minClamp = std::numeric_limits<uint8_t>::lowest();
        maxClamp = std::numeric_limits<uint8_t>::max();
    }

    auto clampedValue = std::clamp(scaledValue, minClamp, maxClamp);

    // This works for both signed and unsigned,
    // because it is the same underlying byte storage.
    // 这对有符号和无符号都适用，
    // 因为底层的字节存储是相同的。
    return static_cast<uint8_t>(clampedValue);
}

static inline uint8_t getScaledIPMIValue(const double value, const double max,
                                         const double min)
{
    int16_t mValue = 0;
    int8_t rExp = 0;
    int16_t bValue = 0;
    int8_t bExp = 0;
    bool bSigned = false;

    bool result =
        getSensorAttributes(max, min, mValue, rExp, bValue, bExp, bSigned);
    if (!result)
    {
        throw std::runtime_error("Illegal sensor attributes");
    }

    return scaleIPMIValueFromDouble(value, mValue, rExp, bValue, bExp, bSigned);
}

} // namespace ipmi
