#include "delivery_types.h"

#include <algorithm>

namespace notch_mock
{
const std::vector<DeliveryTypeContract>& DeliveryTypes()
{
    static const std::vector<DeliveryTypeContract> kTypes = {
        {"image", "IMAGE", "image", "image.png", true, VerificationClass::DecodedImage},
        {"audio", "AUDIO", "audio", "audio.wav", false, VerificationClass::Integrity},
        {"video", "VIDEO", "video", "video.mp4", false, VerificationClass::Integrity},
        {"file_3d", "FILE_3D", "file_3d", "mesh.glb", false, VerificationClass::ByteExact},
        {"mesh", "MESH", "mesh", "mesh.glb", false, VerificationClass::Integrity},
        {"load3d_camera", "LOAD3D_CAMERA", "load3d_camera", "", false, VerificationClass::Structural},
        {"file_path", "STRING", "file_path", "mesh.glb", false, VerificationClass::ByteExact},
    };
    return kTypes;
}

std::vector<std::string> TypeAllowedTransports(const DeliveryTypeContract& type)
{
    if (type.isImage)
    {
        return {"cuda", "disk", "http"};
    }
    return {"disk", "http"};
}

std::vector<std::string> TopologyReachable(Phase phase)
{
    if (phase == Phase::DeliveryRemote)
    {
        return {"http"};
    }
    return {"cuda", "disk", "http"};
}

std::vector<std::string> UsableTransports(const DeliveryTypeContract& type,
                                          const std::vector<std::string>& serverAvailable,
                                          const std::vector<std::string>& clientReachable)
{
    std::vector<std::string> usable;
    for (const std::string& transport : TypeAllowedTransports(type))
    {
        const bool inServer =
            std::find(serverAvailable.begin(), serverAvailable.end(), transport) != serverAvailable.end();
        const bool inClient =
            std::find(clientReachable.begin(), clientReachable.end(), transport) != clientReachable.end();
        if (inServer && inClient)
        {
            usable.push_back(transport);
        }
    }
    return usable;
}
}
