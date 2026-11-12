// Float to integer conversion as the original's x87 code does it.
#pragma once

#include <cmath>
#include <cstdint>

namespace ls3d {

// MSVC 6 __ftol (0x1008d488): truncates toward zero with a 64-bit FISTP. NaN and values outside
// the 64-bit range give the "integer indefinite" 0x8000000000000000; callers keep the low 32 bits,
// so those come out as 0.
inline int32_t Ftol(double value)
{
    if (!(value > -9223372036854775808.0 && value < 9223372036854775808.0))
        return 0;
    return static_cast<int32_t>(static_cast<int64_t>(value));
}

// FISTP with the FPU's rounding mode (to nearest, ties to even): the compiler's inline conversions
// (color * 255 ...). NaN and values outside the 32-bit range give the "integer indefinite"
// 0x80000000.
inline int32_t Fistp(float value)
{
    const float r = std::nearbyint(value);
    if (!(r >= -2147483648.0f && r < 2147483648.0f))
        return INT32_MIN;
    return static_cast<int32_t>(r);
}

// The x87 status word (FNSTSW).
inline uint16_t X87StatusWord()
{
    uint16_t sw;
#if defined(_MSC_VER) && defined(_M_IX86)
    __asm fnstsw sw
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    __asm__ __volatile__("fnstsw %0" : "=m"(sw));
#else
    sw = 0;
#endif
    return sw;
}

}  // namespace ls3d
