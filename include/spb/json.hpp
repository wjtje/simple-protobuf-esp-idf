/***************************************************************************\
* Name        : Public API for JSON                                         *
* Description : all json serialize and deserialize functions                *
* Author      : antonin.kriz@gmail.com                                      *
* ------------------------------------------------------------------------- *
* This is free software; you can redistribute it and/or modify it under the *
* terms of the MIT license. A copy of the license can be found in the file  *
* "LICENSE" at the root of this distribution.                               *
\***************************************************************************/
#pragma once

#include "spb/io/io.hpp"
#include "json/deserialize.hpp"
#include "json/serialize.hpp"
#include <cstdlib>
#include <expected>

namespace spb::json
{

/**
 * @brief serialize message via writer
 *
 * @param message to be serialized
 * @param on_write function for handling the writes
 * @return serialized size in bytes
 * @throws exceptions only from `on_write`
 */
[[nodiscard]] static inline std::expected<size_t, esp_err_t> serialize( const auto & message, spb::io::writer on_write ) noexcept
{
    size_t size;
    const esp_err_t ret = detail::serialize( message, on_write, size );
    if (ret != ESP_OK)
        return std::unexpected(ret);
    return size;
}

/**
 * @brief return json-string serialized size in bytes
 *
 * @param message to be serialized
 * @return serialized size in bytes
 */
[[nodiscard]] static inline size_t serialize_size( const auto & message ) noexcept
{
    return serialize( message, spb::io::writer( nullptr ) ).value_or(0);
}

/**
 * @brief serialize message into json-string
 *
 * @param message to be serialized
 * @return serialized json
 * @throws std::runtime_error on error
 * @example `auto serialized = std::vector< std::byte >();`
 *          `spb::json::serialize( message, serialized );`
 */
template < typename Message, spb::resizable_container Container >
[[nodiscard]] static inline std::expected<size_t, esp_err_t> serialize( const Message & message, Container & result ) noexcept
{
    const auto size = serialize_size( message );
    result.resize( size );
    auto writer = [ ptr = result.data( ) ]( const void * data, size_t size ) mutable -> esp_err_t
    {
        memcpy( ptr, data, size );
        ptr += size;
        return ESP_OK;
    };

    return serialize( message, writer );
}

/**
 * @brief serialize message into json
 *
 * @param[in] message to be serialized
 * @return serialized json
 * @throws std::runtime_error on error
 * @example `auto serialized_message = spb::json::serialize( message );`
 */
template < spb::resizable_container Container = std::string, typename Message >
[[nodiscard]] static inline std::expected<Container, esp_err_t> serialize( const Message & message ) noexcept
{
    auto c = Container( );
    auto result = serialize< Message, Container >( message, c );
    if (result.has_value())
        return c;
    return std::unexpected(result.error());
}

/**
 * @brief deserialize json-string into variable
 *
 * @param on_read function for handling reads
 * @param result deserialized json
 * @throws std::runtime_error on error
 */
[[nodiscard]] static inline esp_err_t deserialize( auto & result, spb::io::reader on_read ) noexcept
{
    return detail::deserialize( result, on_read );
}

/**
 * @brief deserialize json-string into variable
 *
 * @param json string with json
 * @param message deserialized json
 * @throws std::runtime_error on error
 * @example `auto serialized = std::string( ... );`
 *          `auto message = Message();`
 *          `spb::json::deserialize( message, serialized );`
 */
template < typename Message, spb::size_container Container >
[[nodiscard]] static inline esp_err_t deserialize( Message & message, const Container & json ) noexcept
{
    auto reader = [ ptr = json.data( ), end = json.data( ) + json.size( ) ](
                      void * data, size_t size ) mutable -> size_t
    {
        size_t bytes_left = end - ptr;

        size = std::min( size, bytes_left );
        memcpy( data, ptr, size );
        ptr += size;
        return size;
    };
    return deserialize( message, reader );
}

/**
 * @brief deserialize json-string into variable
 *
 * @param json string with json
 * @return deserialized json or throw an exception
 * @example `auto serialized = std::string( ... );`
 *          `auto message = spb::json::deserialize< Message >( serialized );`
 */
template < typename Message, spb::size_container Container >
[[nodiscard]] static inline std::expected<Message, esp_err_t> deserialize( const Container & json ) noexcept
{
    auto message = Message{ };
    if (esp_err_t ret = deserialize( message, json ); unlikely(ret != ESP_OK))
        return std::unexpected(ret);
    return message;
}

/**
 * @brief deserialize json-string into variable
 *
 * @param on_read function for handling reads
 * @return deserialized json
 * @throws std::runtime_error on error
 * @example `auto message = spb::json::deserialize< Message >( reader )`
 */
template < typename Message >
[[nodiscard]] static inline std::expected<Message, esp_err_t> deserialize( spb::io::reader on_read )
{
    auto message = Message{ };
    return deserialize( message, on_read );
}

}// namespace spb::json
