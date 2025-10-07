#pragma once

#include <mega/common/error_or.h>
#include <mega/types.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace mega
{
namespace common
{

std::chrono::minutes defaultTimeout();

std::string format(const char* format, ...);

std::string formatv(std::va_list arguments, const char* format);

std::optional<std::string> fromCharPointer(const char* maybeString);

template<typename T>
using SharedPromise = std::shared_ptr<std::promise<T>>;

template<typename T>
SharedPromise<T> makeSharedPromise()
{
    return std::make_shared<std::promise<T>>();
}

std::int64_t now();

const char* toCharPointer(const std::optional<std::string>& maybeString);

template<typename T>
auto waitFor(std::future<T> future)
  -> typename std::enable_if<IsErrorLike<T>::value, T>::type
{
    // Wait for the future's promise to transmit a value.
    auto status = future.wait_for(defaultTimeout());

    // Promise didn't transmit a value in time.
    if (status == std::future_status::timeout)
    {
        if constexpr (IsErrorOr<T>::value)
            return unexpected(LOCAL_ETIMEOUT);
        else
            return LOCAL_ETIMEOUT;
    }

    // Return the promise's value to the caller.
    return future.get();
}

} // common
} // mega

