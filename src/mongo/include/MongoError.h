#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace mongo
{
    class MongoError final : public std::runtime_error
    {
        public:
            MongoError(std::string operation, int code, std::string message)
                : std::runtime_error(std::move(message)),
                  operation_(std::move(operation)),
                  code_(code)
            {
            }

            const std::string& operation() const noexcept
            {
                return operation_;
            }

            int code() const noexcept
            {
                return code_;
            }

        private:
            std::string operation_;
            int code_;
    };

} // namespace mongo
