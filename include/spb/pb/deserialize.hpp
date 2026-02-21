/***************************************************************************\
* Name        : deserialize library for protobuf                            *
* Description : all protobuf deserialization functions                      *
* Author      : antonin.kriz@gmail.com                                      *
* ------------------------------------------------------------------------- *
* This is free software; you can redistribute it and/or modify it under the *
* terms of the MIT license. A copy of the license can be found in the file  *
* "LICENSE" at the root of this distribution.                               *
\***************************************************************************/

#pragma once

#include <esp_err.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <spb/io/io.hpp>
#include <string_view>
#include <type_traits>

#include "../bits.h"
#include "../concepts.h"
#include "../utf8.h"
#include "wire-types.h"

#ifndef SPB_CHECK
#define SPB_CHECK(x)                                         \
  if (esp_err_t ret = x; unlikely(ret != ESP_OK)) {          \
    printf("%s:%i err: %i\n", __ASSERT_FUNC, __LINE__, ret); \
    return ret;                                              \
  }
#endif

namespace spb::pb::detail {

struct istream {
 private:
  spb::io::reader on_read;
  size_t m_size;

 public:
  istream(
    spb::io::reader reader, size_t size = std::numeric_limits<size_t>::max()
  ) noexcept
      : on_read(reader), m_size(size) {}

  [[nodiscard]] esp_err_t skip(uint32_t tag) noexcept;
  [[nodiscard]] esp_err_t read_skip(size_t size) noexcept;

  [[nodiscard]] esp_err_t deserialize(auto &value, uint32_t tag) noexcept;

  template <scalar_encoder encoder>
  [[nodiscard]] esp_err_t deserialize_as(auto &value, uint32_t tag) noexcept;

  template <size_t ordinal, typename T>
  [[nodiscard]] esp_err_t deserialize_variant(
    T &variant, uint32_t tag
  ) noexcept;

  template <size_t ordinal, scalar_encoder encoder, typename T>
  [[nodiscard]] esp_err_t deserialize_variant_as(
    T &variant, uint32_t tag
  ) noexcept;

  template <scalar_encoder encoder, typename T>
  [[nodiscard]] std::expected<T, esp_err_t> deserialize_bitfield_as(
    uint32_t bits, uint32_t tag
  ) noexcept;

  [[nodiscard]] std::expected<uint8_t, esp_err_t> read_byte() {
    uint8_t result = {};
    if (esp_err_t ret = read_exact(&result, sizeof(result));
        unlikely(ret != ESP_OK))
      return std::unexpected(ret);
    return result;
  }

  [[nodiscard]] auto read_byte_or_eof() -> int {
    uint8_t result = {};
    if (on_read(&result, sizeof(result)) == 0) {
      return -1;
    }
    return result;
  }

  [[nodiscard]] auto size() const -> size_t { return m_size; }

  [[nodiscard]] esp_err_t read_exact(void *data, size_t size) noexcept {
    if (this->size() < size) [[unlikely]]
      return ESP_ERR_INVALID_SIZE;

    while (size > 0) {
      auto chunk_size = on_read(data, size);
      if (chunk_size == 0) return ESP_ERR_INVALID_SIZE;

      size -= chunk_size;
      m_size -= chunk_size;
    }

    return ESP_OK;
  }

  [[nodiscard]] auto empty() const -> bool { return size() == 0; }

  [[nodiscard]] std::expected<istream, esp_err_t> sub_stream(
    size_t sub_size
  ) noexcept {
    if (size() < sub_size) return std::unexpected(ESP_ERR_INVALID_SIZE);

    m_size -= sub_size;
    return istream(on_read, sub_size);
  }
};

[[nodiscard]] static inline auto wire_type_from_tag(uint32_t tag) -> wire_type {
  return wire_type(tag & 0x07);
}

[[nodiscard]] static inline auto field_from_tag(uint32_t tag) -> uint32_t {
  return tag >> 3;
}

[[nodiscard]] static inline std::expected<uint32_t, esp_err_t> read_tag_or_eof(
  istream &stream
) noexcept {
  auto byte_or_eof = stream.read_byte_or_eof();
  if (byte_or_eof < 0) {
    return 0;
  }
  auto byte = uint8_t(byte_or_eof);
  auto tag = uint32_t(byte & 0x7F);

  for (size_t shift = CHAR_BIT - 1; (byte & 0x80) != 0; shift += CHAR_BIT - 1) {
    if (shift >= sizeof(tag) * CHAR_BIT)
      return std::unexpected(ESP_ERR_INVALID_ARG);

    const auto tmp = stream.read_byte();
    if (unlikely(!tmp.has_value())) return std::unexpected(tmp.error());

    byte = tmp.value();
    tag |= uint64_t(byte & 0x7F) << shift;
  }

  if (unlikely(field_from_tag(tag) == 0))
    return std::unexpected(ESP_ERR_INVALID_ARG);

  return tag;
}

template <typename T>
[[nodiscard]] static inline std::expected<T, esp_err_t> read_varint(
  istream &stream
) noexcept {
  if constexpr (std::is_same_v<T, bool>) {
    switch (stream.read_byte().value_or(UINT8_MAX)) {
      case 0:
        return false;
      case 1:
        return true;
      default:
        return std::unexpected(ESP_ERR_INVALID_ARG);
    }
  } else {
    auto value = uint64_t(0);

    for (auto shift = 0U; shift < sizeof(value) * CHAR_BIT;
         shift += CHAR_BIT - 1) {
      const auto byte = stream.read_byte();
      value |= uint64_t(byte.value() & 0x7F) << shift;
      if ((byte.value() & 0x80) == 0) {
        if constexpr (std::is_signed_v<T> && sizeof(T) < sizeof(value)) {
          //- GPB encodes signed varints always as 64-bits
          //- so int32_t(-2) is encoded as
          //"\xfe\xff\xff\xff\xff\xff\xff\xff\xff\x01",
          // same as int64_t(-2)
          //- but it should be encoded as  "\xfe\xff\xff\xff\x0f"
          value = T(value);
        }
        auto result = T(value);
        if constexpr (std::is_signed_v<T>) {
          if (result == std::make_signed_t<T>(value)) {
            return result;
          }
        } else {
          if (result == value) {
            return result;
          }
        }

        break;
      }
    }
  }

  return std::unexpected(ESP_ERR_INVALID_ARG);
}

[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, spb::detail::proto_message auto &value, wire_type type
) noexcept;

template <scalar_encoder encoder>
[[nodiscard]] static inline esp_err_t deserialize_as(
  istream &stream,
  spb::detail::proto_field_int_or_float auto &value,
  wire_type type
) noexcept;
[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, spb::detail::proto_field_bytes auto &value, wire_type type
) noexcept;
[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, spb::detail::proto_label_repeated auto &value, wire_type type
) noexcept;
[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, spb::detail::proto_field_string auto &value, wire_type type
) noexcept;

template <scalar_encoder encoder, spb::detail::proto_label_repeated C>
[[nodiscard]] static inline esp_err_t deserialize_as(
  istream &stream, C &value, wire_type type
) noexcept;

template <scalar_encoder encoder, typename keyT, typename valueT>
[[nodiscard]] static inline esp_err_t deserialize_as(
  istream &stream, std::map<keyT, valueT> &value, wire_type type
) noexcept;

[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream,
  spb::detail::proto_label_optional auto &p_value,
  wire_type type
) noexcept;

template <scalar_encoder encoder, spb::detail::proto_label_optional C>
[[nodiscard]] static inline esp_err_t deserialize_as(
  istream &stream, C &p_value, wire_type type
) noexcept;

template <typename T>
[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, std::unique_ptr<T> &value, wire_type type
) noexcept;

template <typename T, typename signedT, typename unsignedT>
static auto create_tmp_var() {
  if constexpr (std::is_signed<T>::value) {
    return signedT();
  } else {
    return unsignedT();
  }
}

template <scalar_encoder encoder, typename T>
[[nodiscard]] static inline std::expected<T, esp_err_t> deserialize_bitfield_as(
  istream &stream, uint32_t bits, wire_type type
) noexcept {
  auto value = T();
  if constexpr (type1(encoder) == scalar_encoder::svarint) {
    if (unlikely(type != wire_type::varint))
      return std::unexpected(ESP_ERR_INVALID_ARG);

    auto tmp = read_varint<std::make_unsigned_t<T> >(stream);
    if (unlikely(!tmp.has_value())) return std::unexpected(tmp.error());

    value = T((tmp.value() >> 1) ^ (~(tmp.value() & 1) + 1));
  } else if constexpr (type1(encoder) == scalar_encoder::varint) {
    if (unlikely(type != wire_type::varint))
      return std::unexpected(ESP_ERR_INVALID_ARG);

    auto tmp = read_varint<T>(stream);
    if (unlikely(!tmp.has_value())) return std::unexpected(tmp.error());

    value = tmp.value();
  } else if constexpr (type1(encoder) == scalar_encoder::i32) {
    static_assert(sizeof(T) <= sizeof(uint32_t));
    if (unlikely(type != wire_type::fixed32))
      return std::unexpected(ESP_ERR_INVALID_ARG);

    if constexpr (sizeof(value) == sizeof(uint32_t)) {
      SPB_CHECK(stream.read_exact(&value, sizeof(value)));
    } else {
      auto tmp = create_tmp_var<T, int32_t, uint32_t>();
      SPB_CHECK(stream.read_exact(&tmp, sizeof(tmp)));
      SPB_CHECK(spb::detail::check_if_value_fit_in_bits(tmp, bits));
      value = T(tmp);
    }
  } else if constexpr (type1(encoder) == scalar_encoder::i64) {
    static_assert(sizeof(T) <= sizeof(uint64_t));
    if (unlikely(type != wire_type::fixed64))
      return std::unexpected(ESP_ERR_INVALID_ARG);

    if constexpr (sizeof(value) == sizeof(uint64_t)) {
      SPB_CHECK(stream.read_exact(&value, sizeof(value)));
    } else {
      auto tmp = create_tmp_var<T, int64_t, uint64_t>();
      SPB_CHECK(stream.read_exact(&tmp, sizeof(tmp)));
      SPB_CHECK(spb::detail::check_if_value_fit_in_bits(tmp, bits));
      value = T(tmp);
    }
  }

  SPB_CHECK(spb::detail::check_if_value_fit_in_bits(value, bits));
  return value;
}

template <scalar_encoder encoder>
[[nodiscard]] static inline esp_err_t deserialize_as(
  istream &stream, spb::detail::proto_enum auto &value, wire_type type
) noexcept {
  using T = std::remove_cvref_t<decltype(value)>;
  using int_type = std::underlying_type_t<T>;

  if constexpr (!is_packed(encoder)) {
    if (unlikely(type != wire_type::varint)) return ESP_ERR_INVALID_ARG;
  }

  const auto tmp = read_varint<int_type>(stream);
  if (unlikely(!tmp.has_value())) return tmp.error();

  value = T(tmp.value());
  return ESP_OK;
}

template <scalar_encoder encoder>
[[nodiscard]] static inline esp_err_t deserialize_as(
  istream &stream,
  spb::detail::proto_field_int_or_float auto &value,
  wire_type type
) noexcept {
  using T = std::remove_cvref_t<decltype(value)>;

  if constexpr (type1(encoder) == scalar_encoder::svarint) {
    if constexpr (!is_packed(encoder))
      if (unlikely(type != wire_type::varint)) return ESP_ERR_INVALID_ARG;

    auto tmp = read_varint<std::make_unsigned_t<T> >(stream);
    if (unlikely(!tmp.has_value())) return tmp.error();

    value = T((tmp.value() >> 1) ^ (~(tmp.value() & 1) + 1));
  } else if constexpr (type1(encoder) == scalar_encoder::varint) {
    if constexpr (!is_packed(encoder))
      if (unlikely(type != wire_type::varint)) return ESP_ERR_INVALID_ARG;

    const auto tmp = read_varint<T>(stream);
    if (unlikely(!tmp.has_value())) return tmp.error();

    value = tmp.value();
  } else if constexpr (type1(encoder) == scalar_encoder::i32) {
    static_assert(sizeof(T) <= sizeof(uint32_t));

    if constexpr (!is_packed(encoder)) {
      if (unlikely(type != wire_type::fixed32)) return ESP_ERR_INVALID_ARG;
    }
    if constexpr (sizeof(value) == sizeof(uint32_t)) {
      SPB_CHECK(stream.read_exact(&value, sizeof(value)));
    } else {
      if constexpr (std::is_signed_v<T>) {
        auto tmp = int32_t(0);
        SPB_CHECK(stream.read_exact(&tmp, sizeof(tmp)));
        if (tmp > std::numeric_limits<T>::max() ||
            tmp < std::numeric_limits<T>::min()) {
          return ESP_ERR_INVALID_ARG;
        }
        value = T(tmp);
      } else {
        auto tmp = uint32_t(0);
        SPB_CHECK(stream.read_exact(&tmp, sizeof(tmp)));
        if (tmp > std::numeric_limits<T>::max()) {
          return ESP_ERR_INVALID_ARG;
        }
        value = T(tmp);
      }
    }
  } else if constexpr (type1(encoder) == scalar_encoder::i64) {
    static_assert(sizeof(T) <= sizeof(uint64_t));
    if constexpr (!is_packed(encoder)) {
      if (unlikely(type != wire_type::fixed64)) return ESP_ERR_INVALID_ARG;
    }
    if constexpr (sizeof(value) == sizeof(uint64_t)) {
      SPB_CHECK(stream.read_exact(&value, sizeof(value)));
    } else {
      if constexpr (std::is_signed_v<T>) {
        auto tmp = int64_t(0);
        SPB_CHECK(stream.read_exact(&tmp, sizeof(tmp)));
        if (tmp > std::numeric_limits<T>::max() ||
            tmp < std::numeric_limits<T>::min()) {
          return ESP_ERR_INVALID_ARG;
        }
        value = T(tmp);
      } else {
        auto tmp = uint64_t(0);
        SPB_CHECK(stream.read_exact(&tmp, sizeof(tmp)));
        if (tmp > std::numeric_limits<T>::max()) {
          return ESP_ERR_INVALID_ARG;
        }
        value = T(tmp);
      }
    }
  }

  return ESP_OK;
}

[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, spb::detail::proto_enum auto &value, wire_type type
) noexcept {
  using T = std::remove_cvref_t<decltype(value)>;
  using int_type = std::underlying_type_t<T>;

  if (unlikely(type != wire_type::varint)) return ESP_ERR_INVALID_ARG;

  const auto tmp = read_varint<int_type>(stream);
  if (unlikely(!tmp.has_value())) return tmp.error();

  value = T(tmp.value());
  return ESP_OK;
}

[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream,
  spb::detail::proto_label_optional auto &p_value,
  wire_type type
) noexcept {
  auto &value =
    p_value.emplace(typename std::decay_t<decltype(p_value)>::value_type());
  return deserialize(stream, value, type);
}

template <scalar_encoder encoder, spb::detail::proto_label_optional C>
[[nodiscard]] static inline esp_err_t deserialize_as(
  istream &stream, C &p_value, wire_type type
) noexcept {
  auto &value = p_value.emplace(typename C::value_type());
  return deserialize_as<encoder>(stream, value, type);
}

[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, spb::detail::proto_field_string auto &value, wire_type type
) noexcept {
  if (unlikely(type != wire_type::length_delimited)) return ESP_ERR_INVALID_ARG;

  if constexpr (spb::detail::proto_field_string_resizable<decltype(value)>) {
    value.resize(stream.size());
  } else {
    if (value.size() != stream.size()) {
      return ESP_ERR_INVALID_SIZE;
    }
  }

  SPB_CHECK(stream.read_exact(value.data(), stream.size()));
  if (unlikely(!spb::detail::utf8::is_valid(
        std::string_view(value.data(), value.size())
      )))
    return ESP_ERR_INVALID_ARG;

  return ESP_OK;
}

template <typename T>
[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, std::unique_ptr<T> &value, wire_type type
) noexcept {
  value = std::make_unique<T>();
  return deserialize(stream, *value, type);
}

[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, spb::detail::proto_field_bytes auto &value, wire_type type
) noexcept {
  if (unlikely(type != wire_type::length_delimited)) return ESP_ERR_INVALID_ARG;

  if constexpr (spb::detail::proto_field_bytes_resizable<decltype(value)>) {
    value.resize(stream.size());
  } else {
    if (stream.size() != value.size()) {
      return ESP_ERR_INVALID_SIZE;
    }
  }

  return stream.read_exact(value.data(), stream.size());
}

[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, spb::detail::proto_label_repeated auto &value, wire_type type
) noexcept {
  return deserialize(stream, value.emplace_back(), type);
}

template <scalar_encoder encoder, spb::detail::proto_label_repeated C>
[[nodiscard]] static inline esp_err_t deserialize_packed_as(
  istream &stream, C &value, wire_type type
) noexcept {
  while (!stream.empty()) {
    if constexpr (std::is_same_v<typename C::value_type, bool>) {
      const auto tmp = read_varint<bool>(stream);
      if (unlikely(!tmp.has_value())) return tmp.error();

      value.emplace_back(tmp.value());
    } else {
      SPB_CHECK(deserialize_as<encoder>(stream, value.emplace_back(), type));
    }
  }

  return ESP_OK;
}

template <scalar_encoder encoder, spb::detail::proto_label_repeated C>
[[nodiscard]] static inline esp_err_t deserialize_as(
  istream &stream, C &value, wire_type type
) noexcept {
  if constexpr (is_packed(encoder)) {
    return deserialize_packed_as<encoder>(
      stream, value, wire_type_from_scalar_encoder(encoder)
    );
  } else {
    if constexpr (std::is_same_v<typename C::value_type, bool>) {
      const auto tmp = read_varint<bool>(stream);
      if (unlikely(!tmp.has_value())) return tmp.error();

      value.emplace_back(tmp.value());
    } else {
      return deserialize_as<encoder>(stream, value.emplace_back(), type);
    }
  }

  return ESP_OK;
}

template <scalar_encoder encoder, typename keyT, typename valueT>
[[nodiscard]] static inline esp_err_t deserialize_as(
  istream &stream, std::map<keyT, valueT> &value, wire_type type
) noexcept {
  const auto key_encoder = type1(encoder);
  const auto value_encoder = type2(encoder);

  if (unlikely(type != wire_type::length_delimited)) return ESP_ERR_INVALID_ARG;

  auto pair = std::pair<keyT, valueT>();
  auto key_defined = false;
  auto value_defined = false;
  while (!stream.empty()) {
    const auto tag = read_varint<uint32_t>(stream);
    if (unlikely(!tag.has_value())) return tag.error();

    const auto field = field_from_tag(tag.value());
    const auto field_type = wire_type_from_tag(tag.value());

    switch (field) {
      case 1:
        if constexpr (std::is_integral_v<keyT>) {
          SPB_CHECK(
            deserialize_as<key_encoder>(stream, pair.first, field_type)
          );
        } else {
          if (field_type == wire_type::length_delimited) {
            const auto size = read_varint<uint32_t>(stream);
            if (unlikely(!size.has_value())) return size.error();

            auto substream = stream.sub_stream(size.value());
            if (unlikely(!substream.has_value())) return substream.error();

            SPB_CHECK(deserialize(substream.value(), pair.first, field_type));
            if (unlikely(!stream.empty())) return ESP_ERR_INVALID_SIZE;
          } else {
            SPB_CHECK(deserialize(stream, pair.first, field_type));
          }
        }
        key_defined = true;
        break;
      case 2:
        if constexpr (spb::detail::proto_field_int_or_float<valueT>) {
          SPB_CHECK(
            deserialize_as<value_encoder>(stream, pair.second, field_type)
          );
        } else {
          if (field_type == wire_type::length_delimited) {
            const auto size = read_varint<uint32_t>(stream);
            if (unlikely(!size.has_value())) return size.error();

            auto substream = stream.sub_stream(size.value());
            if (unlikely(!substream.has_value())) return substream.error();

            SPB_CHECK(deserialize(substream.value(), pair.second, field_type));
            if (unlikely(!stream.empty())) return ESP_ERR_INVALID_SIZE;
          } else {
            SPB_CHECK(deserialize(stream, pair.second, field_type));
          }
        }
        value_defined = true;
        break;
      default:
        return ESP_ERR_INVALID_ARG;
    }
  }

  if (key_defined && value_defined) {
    value.insert(std::move(pair));
    return ESP_OK;
  }

  return ESP_ERR_INVALID_ARG;
}

template <size_t ordinal, typename T>
[[nodiscard]] static inline esp_err_t deserialize_variant(
  istream &stream, T &variant, wire_type type
) noexcept {
  return deserialize(stream, variant.template emplace<ordinal>(), type);
}

template <size_t ordinal, scalar_encoder encoder, typename T>
[[nodiscard]] static inline esp_err_t deserialize_variant_as(
  istream &stream, T &variant, wire_type type
) noexcept {
  return deserialize_as<encoder>(
    stream, variant.template emplace<ordinal>(), type
  );
}

[[nodiscard]] static inline esp_err_t deserialize_main(
  istream &stream, spb::detail::proto_message auto &value
) noexcept {
  for (;;) {
    const auto tag = read_tag_or_eof(stream);
    if (unlikely(!tag.has_value())) return tag.error();
    if (!tag.value()) break;

    const auto field_type = wire_type_from_tag(tag.value());
    if (field_type == wire_type::length_delimited) {
      const auto size = read_varint<uint32_t>(stream);
      if (unlikely(!size.has_value())) return size.error();

      auto substream = stream.sub_stream(size.value());
      if (unlikely(!substream.has_value())) return substream.error();

      SPB_CHECK(deserialize_value(substream.value(), value, tag.value()));
      if (unlikely(!stream.empty())) return ESP_ERR_INVALID_SIZE;
    } else {
      SPB_CHECK(deserialize_value(stream, value, tag.value()));
    }
  }
  return ESP_OK;
}

[[nodiscard]] static inline esp_err_t deserialize(
  istream &stream, spb::detail::proto_message auto &value, wire_type type
) noexcept {
  if (unlikely(type != wire_type::length_delimited)) return ESP_ERR_INVALID_ARG;

  while (!stream.empty()) {
    const auto tag = read_varint<uint32_t>(stream);
    if (unlikely(!tag.has_value())) return tag.error();

    const auto field_type = wire_type_from_tag(tag.value());

    if (field_type == wire_type::length_delimited) {
      const auto size = read_varint<uint32_t>(stream);
      if (unlikely(!size.has_value())) return size.error();

      auto substream = stream.sub_stream(size.value());
      if (unlikely(!substream.has_value())) return substream.error();

      SPB_CHECK(deserialize_value(substream.value(), value, tag.value()));
      if (unlikely(!stream.empty())) return ESP_ERR_INVALID_SIZE;
    } else {
      SPB_CHECK(deserialize_value(stream, value, tag.value()));
    }
  }
  return ESP_OK;
}

[[nodiscard]] inline esp_err_t istream::deserialize(
  auto &value, uint32_t tag
) noexcept {
  return detail::deserialize(*this, value, wire_type_from_tag(tag));
}

[[nodiscard]] inline esp_err_t istream::read_skip(size_t size) noexcept {
  uint8_t buffer[64];
  while (size > 0) {
    auto chunk_size = std::min(size, sizeof(buffer));
    SPB_CHECK(read_exact(buffer, chunk_size));
    size -= chunk_size;
  }
  return ESP_OK;
}

[[nodiscard]] inline esp_err_t istream::skip(uint32_t tag) noexcept {
  switch (wire_type_from_tag(tag)) {
    case wire_type::varint:
      return read_varint<uint64_t>(*this).error_or(ESP_OK);
    case wire_type::length_delimited:
      return read_skip(size());
    case wire_type::fixed32:
      return read_skip(sizeof(uint32_t));
    case wire_type::fixed64:
      return read_skip(sizeof(uint64_t));
    default:
      return ESP_ERR_INVALID_ARG;
  }
}

template <scalar_encoder encoder>
[[nodiscard]] inline esp_err_t istream::deserialize_as(
  auto &value, uint32_t tag
) noexcept {
  return detail::deserialize_as<encoder>(*this, value, wire_type_from_tag(tag));
}

template <size_t ordinal, typename T>
[[nodiscard]] inline esp_err_t istream::deserialize_variant(
  T &variant, uint32_t tag
) noexcept {
  return detail::deserialize_variant<ordinal>(
    *this, variant, wire_type_from_tag(tag)
  );
}

template <size_t ordinal, scalar_encoder encoder, typename T>
[[nodiscard]] inline esp_err_t istream::deserialize_variant_as(
  T &variant, uint32_t tag
) noexcept {
  return detail::deserialize_variant_as<ordinal, encoder>(
    *this, variant, wire_type_from_tag(tag)
  );
}

template <scalar_encoder encoder, typename T>
[[nodiscard]] inline std::expected<T, esp_err_t>
istream::deserialize_bitfield_as(uint32_t bits, uint32_t tag) noexcept {
  return detail::deserialize_bitfield_as<encoder, T>(
    *this, bits, wire_type_from_tag(tag)
  );
}

[[nodiscard]] static inline esp_err_t deserialize(
  auto &value, spb::io::reader on_read
) noexcept {
  using T = std::remove_cvref_t<decltype(value)>;
  static_assert(spb::detail::proto_message<T>);

  auto stream = istream(on_read);
  return deserialize_main(stream, value);
}

}  // namespace spb::pb::detail

#undef SPB_CHECK
