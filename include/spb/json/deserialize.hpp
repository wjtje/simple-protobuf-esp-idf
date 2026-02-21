/***************************************************************************\
* Name        : deserialize library for json                                *
* Description : all json deserialization functions                          *
* Author      : antonin.kriz@gmail.com                                      *
* ------------------------------------------------------------------------- *
* This is free software; you can redistribute it and/or modify it under the *
* terms of the MIT license. A copy of the license can be found in the file  *
* "LICENSE" at the root of this distribution.                               *
\***************************************************************************/

#pragma once

#include "../bits.h"
#include "../concepts.h"
#include "../from_chars.h"
#include "../utf8.h"
#include "base64.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <spb/io/buffer-io.hpp>
#include <spb/io/io.hpp>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#ifndef SPB_CHECK
#define SPB_CHECK(x) \
    if (esp_err_t ret = x; unlikely(ret != ESP_OK)) return ret;
#endif

namespace spb::json::detail
{
using namespace std::literals;

static const auto escape = '\\';

/**
 * @brief helper for std::variant visit
 *        https://en.cppreference.com/w/cpp/utility/variant/visit
 *
 */
template < class... Ts >
struct overloaded : Ts...
{
    using Ts::operator( )...;
};
// explicit deduction guide (not needed as of C++20)
template < class... Ts >
overloaded( Ts... ) -> overloaded< Ts... >;

/**
 * @brief djb2_hash for strings
 *        http://www.cse.yorku.ca/~oz/hash.html
 *
 * @param str
 * @return uint32_t
 */
static constexpr inline auto djb2_hash( std::string_view str ) noexcept -> uint32_t
{
    uint32_t hash = 5381U;

    for( auto c : str )
    {
        hash = ( ( hash << 5U ) + hash ) + uint8_t( c ); /* hash * 33 + c */
    }

    return hash;
}

static constexpr inline auto fnv1a_hash( std::string_view str ) noexcept -> uint64_t
{
    uint64_t hash        = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;

    for( auto c : str )
    {
        hash *= prime;
        hash ^= c;
    }

    return hash;
}

template < spb::detail::proto_field_bytes T >
void clear( T & container )
{
    if constexpr( spb::detail::proto_field_bytes_resizable< T > )
    {
        container.clear( );
    }
    else
    {
        std::fill( container.begin( ), container.end( ), typename T::value_type( ) );
    }
}

struct istream
{
private:
    spb::io::buffered_reader reader;

    //- current char
    int m_current = -1;

    std::string_view m_current_key;

    /**
     * @brief gets the next char from the stream
     *
     * @param skip_white_space if true, skip white spaces
     */
    void update_current( bool skip_white_space )
    {
        for( ;; )
        {
            auto view = reader.view( 1 );
            if( view.empty( ) )
            {
                m_current = 0;
                return;
            }
            m_current = view[ 0 ];
            if( !skip_white_space )
            {
                return;
            }
            size_t spaces = 0;
            for( auto c : view )
            {
                if( !isspace( c ) )
                {
                    m_current = c;
                    reader.skip( spaces );
                    return;
                }
                spaces += 1;
            }
            reader.skip( spaces );
        }
    }

    [[nodiscard]] auto eof( ) -> bool
    {
        return current_char( ) == 0;
    }

public:
    istream( spb::io::reader reader )
        : reader( reader )
    {
    }

    [[nodiscard]] esp_err_t deserialize( auto & value ) noexcept;
    template < size_t ordinal, typename T >
    [[nodiscard]] esp_err_t deserialize_variant( T & variant ) noexcept;
    template < typename T >
    [[nodiscard]] std::expected<T,esp_err_t> deserialize_bitfield( uint32_t bits );
    [[nodiscard]] std::expected<int32_t,esp_err_t> deserialize_int( );
    [[nodiscard]] std::expected<std::variant< std::string_view, int32_t >,esp_err_t> deserialize_string_or_int( size_t min_size, size_t max_size );
    [[nodiscard]] std::expected<std::string_view,esp_err_t> deserialize_key( size_t min_size, size_t max_size );
    [[nodiscard]] auto current_key( ) const -> std::string_view;

    [[nodiscard]] char current_char( )
    {
        if( m_current < 0 )
        {
            update_current( true );
        }

        return m_current;
    }
    /**
     * @brief consumes `current char` if its equal to c
     *
     * @param c consumed char
     * @return true if char was consumed
     */
    [[nodiscard]] auto consume( char c ) -> bool
    {
        if( current_char( ) == c )
        {
            consume_current_char( true );
            return true;
        }
        return false;
    }

    /**
     * @brief consumes an `token`
     *
     * @param token consumed `token` (whole word)
     * @return true if `token` was consumed
     */
    [[nodiscard]] auto consume( std::string_view token ) -> bool
    {
        assert( !token.empty( ) );

        if( current_char( ) != token[ 0 ] )
        {
            return false;
        }

        if( !reader.view( token.size( ) ).starts_with( token ) )
        {
            return false;
        }
        auto token_view = reader.view( token.size( ) + 1 ).substr( 0, token.size( ) + 1 );
        if( token_view.size( ) == token.size( ) || isspace( token_view.back( ) ) ||
            ( !isalnum( token_view.back( ) ) && token_view.back( ) != '_' ) )
        {
            reader.skip( token.size( ) );
            update_current( true );
            return true;
        }
        return false;
    }

    [[nodiscard]] std::expected<std::string_view, esp_err_t> view( size_t size ) noexcept
    {
        auto result = reader.view( size );
        if( result.empty( ) )
            return std::unexpected(ESP_ERR_INVALID_ARG);
        return result;
    }

    void consume_current_char( bool skip_white_space ) noexcept
    {
        reader.skip( 1 );
        update_current( skip_white_space );
    }

    void skip( size_t size )
    {
        reader.skip( size );
        m_current = -1;
    }
    [[nodiscard]] esp_err_t skip_value( ) noexcept;
};

[[nodiscard]] static inline esp_err_t deserialize( istream & stream, spb::detail::proto_enum auto & value ) noexcept
{
    return deserialize_value( stream, value );
}

[[nodiscard]] static inline esp_err_t ignore_string( istream & stream ) noexcept
{
    if( stream.current_char( ) != '"' )
        return ESP_ERR_INVALID_ARG;

    auto last = escape;
    for( ;; )
    {
        auto view   = stream.view( UINT32_MAX );
        if (unlikely(!view.has_value())) return view.error();

        auto length = 0U;
        for( auto current : view.value() )
        {
            length += 1;
            if( current == '"' && last != escape )
            {
                stream.skip( length );
                return ESP_OK;
            }
            //- handle \\"
            last = current != escape || last != escape ? current : ' ';
        }
        stream.skip( view->size( ) );
    }

    return ESP_OK;
}

[[nodiscard]] inline std::expected<std::string_view, esp_err_t> deserialize_string_view( istream & stream, size_t min_size,
                                            size_t max_size ) noexcept
{
    if( stream.current_char( ) != '"' )
        return std::unexpected(ESP_ERR_INVALID_STATE);

    //- +2 for '"'
    auto view   = stream.view( max_size + 2 );
    if (unlikely(!view.has_value())) return std::unexpected(view.error());

    auto last   = escape;
    auto length = size_t( 0 );
    for( auto current : view.value() )
    {
        length += 1;

        if( current == '"' && last != escape )
        {
            stream.skip( length );

            if( ( length - 2 ) >= min_size && ( length - 2 ) <= max_size )
            {
                return view->substr( 1, length - 2 );
            }

            return std::string_view();
        }
        //- handle \\"
        last = current != escape || last != escape ? current : ' ';
    }

    if (esp_err_t ret =ignore_string( stream ); unlikely(ret != ESP_OK))
        return std::unexpected(ret);

    return std::string_view();
}

[[nodiscard]] static inline std::expected<uint16_t, esp_err_t> unicode_from_hex( istream & stream ) noexcept
{
    const auto esc_size = 4U;
    auto unicode_view   = stream.view( esc_size );
    if(!unicode_view.has_value() || unicode_view->size( ) < esc_size ){
        return std::unexpected(ESP_ERR_INVALID_ARG);
    }

    auto value = uint16_t( 0 );
    auto result =
        spb_std_emu::from_chars( unicode_view->data( ), unicode_view->data( ) + esc_size, value, 16 );

    if( result.ec != std::errc{ } || result.ptr != unicode_view->data( ) + esc_size ) {
        return std::unexpected(ESP_ERR_INVALID_ARG);
    }

    stream.skip( esc_size );
    return value;
}

[[nodiscard]] static inline std::expected<uint32_t, esp_err_t> unescape_unicode( istream & stream, char utf8[ 4 ] )
{
    auto hex = unicode_from_hex( stream );
    if (unlikely(!hex.has_value())) return std::unexpected(hex.error());

    auto value = uint32_t( hex.value() );
    auto begin = stream.view(2);

    if( value >= 0xD800 && value <= 0xDBFF && begin.has_value() && begin->starts_with( "\\u"sv ) )
    {
        stream.skip( 2 );
        auto hex = unicode_from_hex( stream );
        if (unlikely(!hex.has_value())) return std::unexpected(hex.error());

        auto low = hex.value();
        if( low < 0xDC00 || low > 0xDFFF )
            return std::unexpected(ESP_ERR_INVALID_ARG);
        value = ( ( value - 0xD800 ) << 10 ) + ( low - 0xDC00 ) + 0x10000;
    }
    if( auto result = spb::detail::utf8::encode_point( value, utf8 ); result != 0 )
    {
        return result;
    }
    
    return std::unexpected(ESP_ERR_INVALID_ARG);
}

[[nodiscard]] static inline std::expected<uint32_t, esp_err_t> unescape( istream & stream, char utf8[ 4 ] )
{
    auto c = stream.current_char( );
    stream.consume_current_char( false );
    switch( c )
    {
    case '"':
        utf8[ 0 ] = '"';
        return 1;
    case '\\':
        utf8[ 0 ] = '\\';
        return 1;
    case '/':
        utf8[ 0 ] = '/';
        return 1;
    case 'b':
        utf8[ 0 ] = '\b';
        return 1;
    case 'f':
        utf8[ 0 ] = '\f';
        return 1;
    case 'n':
        utf8[ 0 ] = '\n';
        return 1;
    case 'r':
        utf8[ 0 ] = '\r';
        return 1;
    case 't':
        utf8[ 0 ] = '\t';
        return 1;
    case 'u':
        return unescape_unicode( stream, utf8 );
    default:
        return std::unexpected(ESP_ERR_INVALID_ARG);
    }
}

[[nodiscard]] static inline esp_err_t deserialize( istream & stream, spb::detail::proto_field_string auto & value ) noexcept
{
    if( stream.current_char( ) != '"' )
        return ESP_ERR_INVALID_ARG;

    stream.consume_current_char( false );

    if constexpr( spb::detail::proto_field_string_resizable< decltype( value ) > )
    {
        value.clear( );
    }
    auto index           = size_t( 0 );
    auto append_to_value = [ & ]( const char * str, size_t size ) -> esp_err_t
    {
        if constexpr( spb::detail::proto_field_string_resizable< decltype( value ) > )
        {
            value.append( str, size );
        }
        else
        {
            if( auto space_left = value.size( ) - index; size <= space_left ) [[likely]]
            {
                memcpy( value.data( ) + index, str, size );
                index += size;
            }
            else
            {
                return ESP_ERR_INVALID_SIZE;
            }
        }

        return ESP_OK;
    };

    for( ;; )
    {
        auto view  = stream.view( UINT32_MAX );
        if (unlikely(!view.has_value())) return view.error();

        auto found = view->find_first_of( R"("\)" );
        if( found == view->npos ) [[unlikely]]
        {
            SPB_CHECK(append_to_value( view->data( ), view->size( ) ));
            stream.skip( view->size( ) );
            continue;
        }

        SPB_CHECK(append_to_value( view->data( ), found ));
        // +1 for '"' or '\'
        stream.skip( found + 1 );
        if( view.value()[ found ] == '"' ) [[likely]]
        {
            if constexpr( !spb::detail::proto_field_string_resizable< decltype( value ) > )
            {
                if( index != value.size( ) )
                {
                    return ESP_ERR_INVALID_SIZE;
                }
            }
            return ESP_OK;
        }
        char utf8_buffer[ 4 ];
        auto utf8_size = unescape( stream, utf8_buffer );
        if (unlikely(!utf8_size.has_value())) return utf8_size.error();
        SPB_CHECK(append_to_value( utf8_buffer, utf8_size.value() ));
    }

    if (unlikely(!spb::detail::utf8::is_valid(
          std::string_view(value.data(), value.size())
        )))
      return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

[[nodiscard]] static inline esp_err_t deserialize( istream & stream,
                                spb::detail::proto_field_int_or_float auto & value ) noexcept
{
    if( stream.current_char( ) == '"' ) [[unlikely]]
    {
        //- https://protobuf.dev/programming-guides/proto2/#json
        //- number can be a string
        auto view   = deserialize_string_view( stream, 1, UINT32_MAX );
        if (unlikely(!view.has_value())) return view.error();

        auto result = spb_std_emu::from_chars( view->data( ), view->data( ) + view->size( ), value );
        if( result.ec != std::errc{ } )
            return ESP_ERR_INVALID_ARG;
        return ESP_OK;
    }

    auto view   = stream.view( UINT32_MAX );
    if (unlikely(!view.has_value())) return view.error();

    auto result = spb_std_emu::from_chars( view->data( ), view->data( ) + view->size( ), value );
    if( result.ec != std::errc{ } )
        return ESP_ERR_INVALID_ARG;
    stream.skip( result.ptr - view->data( ) );
    return ESP_OK;
}

[[nodiscard]] static inline esp_err_t deserialize( istream & stream, bool & value ) noexcept
{
    if( stream.consume( "true"sv ) )
    {
        value = true;
    }
    else if( stream.consume( "false"sv ) )
    {
        value = false;
    }
    else
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

[[nodiscard]] static inline esp_err_t deserialize( istream & stream, auto & value ) noexcept;

template < typename keyT, typename valueT >
[[nodiscard]] static inline esp_err_t deserialize( istream & stream, std::map< keyT, valueT > & value ) noexcept;

[[nodiscard]] static inline esp_err_t deserialize( istream & stream, spb::detail::proto_label_optional auto & value ) noexcept;

template < spb::detail::proto_label_repeated C >
[[nodiscard]] static inline esp_err_t deserialize( istream & stream, C & value ) noexcept
{
    if( stream.consume( "null"sv ) )
    {
        value.clear( );
        return ESP_OK;
    }

    if( !stream.consume( '[' ) )
        return ESP_ERR_INVALID_ARG;

    if( stream.consume( ']' ) )
        return ESP_OK;

    do
    {
        if constexpr( std::is_same_v< typename C::value_type, bool > )
        {
            auto b = false;
            SPB_CHECK(deserialize( stream, b ));
            value.push_back( b );
        }
        else
        {
            SPB_CHECK(deserialize( stream, value.emplace_back( ) ));
        }
    } while( stream.consume( ',' ) );

    if( !stream.consume( ']' ) )
        return ESP_ERR_INVALID_ARG;
    
    return ESP_OK;
}

[[nodiscard]] static inline esp_err_t deserialize( istream & stream, spb::detail::proto_field_bytes auto & value ) noexcept
{
    if( stream.consume( "null"sv ) )
    {
        clear( value );
        return ESP_OK;
    }

    return base64_decode_string( value, stream );
}

template < typename T >
[[nodiscard]] esp_err_t deserialize_map_key( istream & stream, T & map_key )
{
    if constexpr( std::is_same_v< T, std::string > )
    {
        return deserialize( stream, map_key );
    }
    auto str_key_map = deserialize_string_view( stream, 1, UINT32_MAX );
    if  (unlikely(!str_key_map.has_value())) return str_key_map.error();

    auto reader = [ ptr = str_key_map->data( ), end = str_key_map->data( ) + str_key_map->size( ) ](
                      void * data, size_t size ) mutable -> size_t
    {
        size_t bytes_left = end - ptr;
        size              = std::min( size, bytes_left );
        memcpy( data, ptr, size );
        ptr += size;
        return size;
    };
    auto key_stream = istream( reader );
    return deserialize( key_stream, map_key );
}

template < typename keyT, typename valueT >
[[nodiscard]] static inline esp_err_t deserialize( istream & stream, std::map< keyT, valueT > & value ) noexcept
{
    if( stream.consume( "null"sv ) )
    {
        value.clear( );
        return ESP_OK;
    }
    if( !stream.consume( '{' ) )
        return ESP_ERR_INVALID_ARG;

    if( stream.consume( '}' ) )
        return ESP_OK;

    do
    {
        auto map_key = keyT( );
        SPB_CHECK(deserialize_map_key( stream, map_key ));
        
        if( !stream.consume( ':' ) )
            return ESP_ERR_INVALID_ARG;

        auto map_value = valueT( );
        SPB_CHECK(deserialize( stream, map_value ));
        value.emplace( std::move( map_key ), std::move( map_value ) );
    } while( stream.consume( ',' ) );

    if( !stream.consume( '}' ) )
        return ESP_ERR_INVALID_ARG;

    return ESP_OK;
}

[[nodiscard]] static inline esp_err_t deserialize( istream & stream, spb::detail::proto_label_optional auto & p_value ) noexcept
{
    if( stream.consume( "null"sv ) )
    {
        p_value.reset( );
        return ESP_OK;
    }

    if( p_value.has_value( ) )
    {
        return deserialize( stream, *p_value );
    }
    else
    {
        return deserialize(
            stream,
            p_value.emplace( typename std::decay_t< decltype( p_value ) >::value_type( ) ) );
    }
}

template < typename T >
[[nodiscard]] static inline esp_err_t deserialize( istream & stream, std::unique_ptr< T > & value ) noexcept
{
    if( stream.consume( "null"sv ) )
    {
        value.reset( );
        return ESP_OK;
    }

    if( value )
    {
        return deserialize( stream, *value );
    }
    else
    {
        value = std::make_unique< T >( );
        return deserialize( stream, *value );
    }
}

[[nodiscard]] static inline esp_err_t ignore_value( istream & stream ) noexcept;

[[nodiscard]] static inline esp_err_t ignore_key_and_value( istream & stream ) noexcept
{
    SPB_CHECK( ignore_string( stream ));
    if( !stream.consume( ':' ) )
        return ESP_ERR_INVALID_ARG;
    return ignore_value( stream );
}

[[nodiscard]] static inline esp_err_t ignore_object( istream & stream ) noexcept
{
    //- '{' was already checked by caller
    stream.consume_current_char( true );

    if( stream.consume( '}' ) )
    {
        return ESP_OK;
    }

    do
    {
        SPB_CHECK(ignore_key_and_value( stream ));
    } while( stream.consume( ',' ) );

    if( !stream.consume( '}' ) )
        return ESP_ERR_INVALID_ARG;

    return ESP_OK;
}

[[nodiscard]] static inline esp_err_t ignore_array( istream & stream ) noexcept
{
    //- '[' was already checked by caller
    stream.consume_current_char( true );

    if( stream.consume( ']' ) )
    {
        return ESP_OK;
    }

    do
    {
        SPB_CHECK(ignore_key_and_value( stream ));
    } while( stream.consume( ',' ) );

    if( !stream.consume( ']' ) )
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

[[nodiscard]] static inline esp_err_t ignore_number( istream & stream ) noexcept
{
    auto value = double{ };
    return deserialize( stream, value );
}

[[nodiscard]] static inline esp_err_t ignore_bool( istream & stream ) noexcept
{
    auto value = bool{ };
    return deserialize( stream, value );
}

[[nodiscard]] static inline esp_err_t ignore_null( istream & stream ) noexcept
{
    if( !stream.consume( "null"sv ) )
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

[[nodiscard]] static inline esp_err_t ignore_value( istream & stream ) noexcept
{
    switch( stream.current_char( ) )
    {
    case '{':
        return ignore_object( stream );
    case '[':
        return ignore_array( stream );
    case '"':
        return ignore_string( stream );
    case 'n':
        return ignore_null( stream );
    case 't':
    case 'f':
        return ignore_bool( stream );
    default:
        return ignore_number( stream );
    }
}

template < typename T >
inline auto deserialize_bitfield( istream & stream, uint32_t bits ) -> T
{
    auto value = T( );
    deserialize( stream, value );
    spb::detail::check_if_value_fit_in_bits( value, bits );
    return value;
}

template < size_t ordinal, typename T >
[[nodiscard]] static inline esp_err_t deserialize_variant( istream & stream, T & variant ) noexcept
{
    return deserialize( stream, variant.template emplace< ordinal >( ) );
}

[[nodiscard]] static inline esp_err_t deserialize( istream & stream, auto & value ) noexcept
{
    if( !stream.consume( '{' ) )
        return ESP_ERR_INVALID_ARG;

    if( stream.consume( '}' ) )
        return ESP_OK;

    for( ;; )
    {
        //
        //- deserialize_value is generated by the sprotoc
        //
        SPB_CHECK(deserialize_value( stream, value ));

        if( stream.consume( ',' ) )
            continue;

        if( stream.consume( '}' ) )
            return ESP_OK;

        return ESP_ERR_INVALID_ARG;
    }
}

inline std::expected<std::string_view,esp_err_t> istream::deserialize_key( size_t min_size, size_t max_size )
{
    auto result = deserialize_string_view( *this, min_size, max_size );
    if (unlikely(!result.has_value())) return std::unexpected(result.error());

    m_current_key = result.value();
    if( !consume( ':' ) )
        return std::unexpected(ESP_ERR_INVALID_ARG);

    return m_current_key;
}

[[nodiscard]] inline esp_err_t istream::deserialize( auto & value ) noexcept
{
    return detail::deserialize( *this, value );
}

template < size_t ordinal, typename T >
[[nodiscard]] inline esp_err_t istream::deserialize_variant( T & variant ) noexcept
{
    return detail::deserialize_variant< ordinal >( *this, variant );
}

template < typename T >
inline std::expected<T, esp_err_t> istream::deserialize_bitfield( uint32_t bits )
{
    return detail::deserialize_bitfield< T >( *this, bits );
}

inline std::expected<std::variant< std::string_view, int32_t >,esp_err_t> istream::deserialize_string_or_int( size_t min_size, size_t max_size )
{
    if( current_char( ) == '"' )
    {
        return deserialize_string_view( *this, min_size, max_size );
    }
    return deserialize_int( );
}

inline std::expected<int32_t,esp_err_t>  istream::deserialize_int( )
{
    auto result = int32_t{ };
    if (esp_err_t ret = detail::deserialize( *this, result ); unlikely(ret != ESP_OK))
        return std::unexpected(ret);
    return result;
}

inline esp_err_t istream::skip_value( ) noexcept
{
    return detail::ignore_value( *this );
}

[[nodiscard]] static inline esp_err_t deserialize( auto & value, spb::io::reader reader ) noexcept
{
    auto stream = detail::istream( reader );
    return detail::deserialize( stream, value );
}

} // namespace spb::json::detail

#undef SPB_CHECK
