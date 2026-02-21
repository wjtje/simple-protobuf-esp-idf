/***************************************************************************\
* Name        : bitfields                                                   *
* Description : bitfields checks                                            *
* Author      : antonin.kriz@gmail.com                                      *
* ------------------------------------------------------------------------- *
* This is free software; you can redistribute it and/or modify it under the *
* terms of the MIT license. A copy of the license can be found in the file  *
* "LICENSE" at the root of this distribution.                               *
\***************************************************************************/
#pragma once

#include <esp_err.h>

#include <cassert>
#include <climits>
#include <cstdint>

namespace spb::detail {
template <typename T>
concept signed_int = std::is_signed_v<T> && std::is_integral_v<T>;

template <typename T>
concept unsigned_int = !std::is_signed_v<T> && std::is_integral_v<T>;

[[nodiscard]] static inline esp_err_t check_if_value_fit_in_bits(
  signed_int auto value, uint32_t bits
) noexcept {
  assert(sizeof(value) * CHAR_BIT >= bits);
  assert(bits > 0);

  decltype(value) max = (1LL << (bits - 1)) - 1;
  decltype(value) min = -(1LL << (bits - 1));

  if ((value < min) | (value > max)) [[unlikely]]
    return ESP_ERR_INVALID_STATE;

  return ESP_OK;
}

[[nodiscard]] static inline esp_err_t check_if_value_fit_in_bits(
  unsigned_int auto value, uint32_t bits
) noexcept {
  assert(sizeof(value) * CHAR_BIT >= bits);

  decltype(value) max = (1LL << bits) - 1;

  if (value > max) [[unlikely]]
    return ESP_ERR_INVALID_STATE;

  return ESP_OK;
}

}  // namespace spb::detail