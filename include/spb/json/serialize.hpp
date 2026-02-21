/***************************************************************************\
* Name        : serialize library for json                                  *
* Description : all json serialization functions                            *
* Author      : antonin.kriz@gmail.com                                      *
* ------------------------------------------------------------------------- *
* This is free software; you can redistribute it and/or modify it under the *
* terms of the MIT license. A copy of the license can be found in the file  *
* "LICENSE" at the root of this distribution.                               *
\***************************************************************************/

#pragma once

#include "../concepts.h"
#include "base64.h"
#include "spb/json/deserialize.hpp"
#include "spb/utf8.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cinttypes>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <spb/io/io.hpp>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <type_traits>
#include <vector>

#ifndef SPB_CHECK
#define SPB_CHECK(x) \
    if (esp_err_t ret = x; unlikely(ret != ESP_OK)) return ret;
#endif

namespace spb::json::detail
{
struct ostream
{
private:
    size_t bytes_written = 0;
    spb::io::writer on_write;

public:
    //- flag if put ',' before value
    bool put_comma = false;

    /**
     * @brief Construct a new ostream object
     *
     * @param writer if null, stream will skip all writes but will still count number of written
     * chars
     */
    explicit ostream( spb::io::writer writer ) noexcept
        : on_write( writer )
    {
    }

    [[nodiscard]] esp_err_t write( char c ) noexcept
    {
        if( on_write )
        {
            SPB_CHECK(on_write( &c, sizeof( c ) ));
        }

        bytes_written += sizeof( c );
        return ESP_OK;
    }

    [[nodiscard]] esp_err_t write_unicode( uint32_t codepoint ) noexcept
    {
        if( codepoint <= 0xffff )
        {
            char buffer[ 8 ] = { };
            auto size        = snprintf( buffer, sizeof( buffer ), "\\u%04" PRIx32, codepoint );
            return write( std::string_view( buffer, size ) );
        }
        if( codepoint <= 0x10FFFF )
        {
            codepoint -= 0x10000;

            auto high         = static_cast< uint16_t >( ( codepoint >> 10 ) + 0xD800 );
            auto low          = static_cast< uint16_t >( ( codepoint & 0x3FF ) + 0xDC00 );
            char buffer[ 16 ] = { };
            auto size         = snprintf( buffer, sizeof( buffer ), "\\u%04x\\u%04x", high, low );
            return write( std::string_view( buffer, size ) );
        }
        return ESP_ERR_INVALID_ARG;
    }

    [[nodiscard]] esp_err_t write( std::string_view str ) noexcept
    {
        if( on_write )
        {
            SPB_CHECK(on_write( str.data( ), str.size( ) ));
        }

        bytes_written += str.size( );
        return ESP_OK;
    }

    [[nodiscard]] esp_err_t write_escaped( std::string_view str ) noexcept
    {
        if( !has_escape_chars( str ) )
        {
            return write( str );
        }

        using namespace std::literals;
        uint32_t codepoint = 0;
        uint32_t state     = spb::detail::utf8::ok;
        bool decoding_utf8 = false;
        for( uint8_t c : str )
        {
            if( decoding_utf8 )
            {
                if( spb::detail::utf8::decode_point( &state, &codepoint, c ) ==
                    spb::detail::utf8::ok )
                {
                    SPB_CHECK(write_unicode( codepoint ));
                    decoding_utf8 = false;
                }
                continue;
            }
            if( is_escape( c ) )
            {
                switch( c )
                {
                case '"':
                    SPB_CHECK(write( R"(\")"sv ));
                    break;
                case '\\':
                    SPB_CHECK(write( R"(\\)"sv ));
                    break;
                case '\b':
                    SPB_CHECK(write( R"(\b)"sv ));
                    break;
                case '\f':
                    SPB_CHECK(write( R"(\f)"sv ));
                    break;
                case '\n':
                    SPB_CHECK(write( R"(\n)"sv ));
                    break;
                case '\r':
                    SPB_CHECK(write( R"(\r)"sv ));
                    break;
                case '\t':
                    SPB_CHECK(write( R"(\t)"sv ));
                    break;
                default:
                    decoding_utf8 = true;
                    if( spb::detail::utf8::decode_point( &state, &codepoint, c ) ==
                        spb::detail::utf8::ok )
                    {
                        SPB_CHECK(write_unicode( codepoint ));
                        decoding_utf8 = false;
                    }
                }
            }
            else
            {
                SPB_CHECK(write( c ));
            }
        }
        if( state != spb::detail::utf8::ok )
        {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    void serialize( std::string_view key, const auto & value );
    void serialize( std::string_view value );

    [[nodiscard]] auto size( ) const noexcept -> size_t
    {
        return bytes_written;
    }

private:
    static auto is_escape( uint8_t c ) -> bool
    {
        static constexpr std::string_view escape_chars = "\\\"\b\f\n\r\t<>";
        return c <= 0x1f || c >= 0x7f || escape_chars.find( c ) != std::string_view::npos;
    }

    static auto has_escape_chars( std::string_view str ) -> bool
    {
        return std::any_of( str.begin( ), str.end( ), is_escape );
    }
};

using namespace std::literals;

static inline esp_err_t serialize_key( ostream & stream, std::string_view key ) noexcept
{
    if( stream.put_comma )
    {
        SPB_CHECK(stream.write( ',' ));
    }
    stream.put_comma = true;

    if( !key.empty( ) )
    {
        SPB_CHECK(stream.write( '"' ));
        SPB_CHECK(stream.write_escaped( key ));
        SPB_CHECK(stream.write( R"(":)"sv ));
    }
    return ESP_OK;
}

static inline esp_err_t serialize( ostream & stream, std::string_view key, const bool & value ) noexcept;
static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_field_int_or_float auto & value ) noexcept;
static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_message auto & value ) noexcept;
static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_enum auto & value ) noexcept;
static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_field_string auto & value ) noexcept;
static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_field_bytes auto & value ) noexcept;
static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_label_repeated auto & value ) noexcept;
template < typename keyT, typename valueT >
static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const std::map< keyT, valueT > & map ) noexcept;

static inline esp_err_t serialize( ostream & stream, bool value ) noexcept;
static inline esp_err_t serialize( ostream & stream, spb::detail::proto_field_int_or_float auto value ) noexcept;
static inline esp_err_t serialize( ostream & stream,
                              const spb::detail::proto_field_string auto & value ) noexcept;

static inline esp_err_t serialize( ostream & stream, bool value ) noexcept
{
    return stream.write( value ? "true"sv : "false"sv );
}

static inline esp_err_t serialize( ostream & stream, const std::string_view & value ) noexcept
{
    SPB_CHECK(stream.write( '"' ));
    SPB_CHECK(stream.write_escaped( value ));
    return stream.write( '"' );
}

static inline esp_err_t serialize( ostream & stream, const spb::detail::proto_field_string auto & value ) noexcept
{
    return serialize( stream, std::string_view( value.data( ), value.size( ) ) );
}

static inline esp_err_t serialize( ostream & stream, spb::detail::proto_field_int_or_float auto value ) noexcept
{
    auto buffer = std::array< char, 32 >( );

    auto result = std::to_chars( buffer.data( ), buffer.data( ) + sizeof( buffer ), value );
    return stream.write(
        std::string_view( buffer.data( ), static_cast< size_t >( result.ptr - buffer.data( ) ) ) );
}

static inline esp_err_t serialize( ostream & stream, std::string_view key, const bool & value ) noexcept
{
    SPB_CHECK(serialize_key( stream, key ));
    return serialize( stream, value );
}

static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_field_int_or_float auto & value ) noexcept
{
    SPB_CHECK(serialize_key( stream, key ));
    return serialize( stream, value );
}

static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_field_string auto & value ) noexcept
{
    if( !value.empty( ) )
    {
        SPB_CHECK(serialize_key( stream, key ));
        return serialize( stream, value );
    }

    return ESP_OK;
}

static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_field_bytes auto & value ) noexcept
{
    if( !value.empty( ) )
    {
        SPB_CHECK(serialize_key( stream, key ));
        SPB_CHECK(stream.write( '"' ));
        SPB_CHECK(base64_encode( stream, value ));
        return stream.write( '"' );
    }

    return ESP_OK;
}

static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_label_repeated auto & value ) noexcept
{
    if( value.empty( ) )
    {
        return ESP_OK;
    }

    SPB_CHECK(serialize_key( stream, key ));
    SPB_CHECK(stream.write( '[' ));
    stream.put_comma = false;
    for( const auto & v : value )
    {
        if constexpr( std::is_same_v< typename std::decay_t< decltype( value ) >::value_type,
                                      bool > )
        {
            SPB_CHECK(serialize( stream, { }, bool( v ) ));
        }
        else
        {
            SPB_CHECK(serialize( stream, { }, v ));
        }
    }
    SPB_CHECK(stream.write( ']' ));
    stream.put_comma = true;
    return ESP_OK;
}

static constexpr std::string_view no_name = { };

template < typename keyT, typename valueT >
static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const std::map< keyT, valueT > & map ) noexcept
{
    if( map.empty( ) )
    {
        return ESP_OK;
    }
    SPB_CHECK(serialize_key( stream, key ));
    SPB_CHECK(stream.write( '{' ));
    stream.put_comma = false;
    for( auto & [ map_key, map_value ] : map )
    {
        if constexpr( std::is_same_v< keyT, std::string_view > ||
                      std::is_same_v< keyT, std::string > )
        {
            SPB_CHECK(serialize_key( stream, map_key ));
        }
        else
        {
            if( stream.put_comma )
            {
                SPB_CHECK(stream.write( ',' ));
            }

            SPB_CHECK(stream.write( '"' ));
            SPB_CHECK(serialize( stream, map_key ));
            SPB_CHECK(stream.write( R"(":)"sv ));
        }
        stream.put_comma = false;
        SPB_CHECK(serialize( stream, no_name, map_value ));
        stream.put_comma = true;
    }
    SPB_CHECK(stream.write( '}' ));
    stream.put_comma = true;
    return ESP_OK;
}

static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_label_optional auto & p_value ) noexcept
{
    if( p_value.has_value( ) )
    {
        return serialize( stream, key, *p_value );
    }
    return ESP_OK;
}

template < typename T >
static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const std::unique_ptr< T > & p_value ) noexcept
{
    if( p_value )
    {
        return serialize( stream, key, *p_value );
    }
    return ESP_OK;
}

static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_message auto & value ) noexcept
{
    SPB_CHECK(serialize_key( stream, key ));
    SPB_CHECK(stream.write( '{' ));
    stream.put_comma = false;

    //
    //- serialize_value is generated by the spb-protoc
    //
    SPB_CHECK(serialize_value( stream, value ));
    SPB_CHECK(stream.write( '}' ));
    stream.put_comma = true;
    return ESP_OK;
}

static inline esp_err_t serialize( ostream & stream, std::string_view key,
                              const spb::detail::proto_enum auto & value ) noexcept
{
    SPB_CHECK(serialize_key( stream, key ));

    //
    //- serialize_value is generated by the spb-protoc
    //
    return serialize_value( stream, value );
}

static inline esp_err_t serialize( const auto & value, spb::io::writer on_write, size_t &size ) noexcept
{
    auto stream = ostream( on_write );
    SPB_CHECK(serialize( stream, no_name, value ));
    size = stream.size();
    return ESP_OK;
}

void ostream::serialize( std::string_view key, const auto & value )
{
    detail::serialize( *this, key, value );
}

inline void ostream::serialize( std::string_view value )
{
    detail::serialize( *this, value );
}

}// namespace spb::json::detail

#undef SPB_CHECK
