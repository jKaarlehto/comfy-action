#ifndef NOTCH_MOCK_TIMED_HTTP_TRANSPORT_H
#define NOTCH_MOCK_TIMED_HTTP_TRANSPORT_H

#include "comfy_extension_client/client.hpp"

namespace notch_mock
{
// Retrieval time excludes the harness evidence log.
class TimedHttpTransport : public ComfyExtensionClient::HttpTransport
{
public:
    virtual double LastTransferMilliseconds() const = 0;
};
}

#endif
