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
        // file_3d uploads bytes (multipart) but save_to reserializes the container,
        // so it is structural, not byte-exact. file_path is the only byte-exact case
        // (an exact copy2 of a server-staged file).
        {"file_3d", "FILE_3D", "file_3d", "mesh.glb", false, VerificationClass::Structural},
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

namespace
{
bool Contains(const std::vector<std::string>& values, const std::string& target)
{
    return std::find(values.begin(), values.end(), target) != values.end();
}
}  // namespace

std::vector<std::string> UsableTransports(const DeliveryTypeContract& type,
                                          const std::vector<std::string>& serverAvailable,
                                          const std::vector<std::string>& clientReachable)
{
    std::vector<std::string> usable;
    for (const std::string& transport : TypeAllowedTransports(type))
    {
        if (Contains(serverAvailable, transport) && Contains(clientReachable, transport))
        {
            usable.push_back(transport);
        }
    }
    return usable;
}

std::vector<DeliveryCaseDescriptor> GenerateDeliveryCases(const TopologyConfig& topology)
{
    static const char* const kTransports[] = {"cuda", "disk", "http"};  // Σ
    std::vector<DeliveryCaseDescriptor> cases;
    for (const DeliveryTypeContract& type : DeliveryTypes())
    {
        for (const char* const* it = kTransports; it != kTransports + 3; ++it)
        {
            const std::string transport = *it;
            const bool allowed = Contains(TypeAllowedTransports(type), transport);
            const bool serverOk = Contains(topology.serverAvailable, transport);
            const bool clientOk = Contains(topology.clientReachable, transport);
            const bool usable = allowed && serverOk && clientOk;

            std::string verdict;
            if (usable)
            {
                verdict = "deliver";
            }
            else if (!allowed)
            {
                // Type-axis exclusion (cuda for a non-image) is topology-independent,
                // so it is proved once, in the local topology, not re-tested per remote.
                if (topology.name != "local")
                {
                    continue;
                }
                verdict = "reject-type";
            }
            else if (!serverOk)
            {
                verdict = "reject-server";
            }
            else
            {
                verdict = "reject-client";
            }

            DeliveryCaseDescriptor descriptor;
            descriptor.id = topology.name + "." + type.outputType + "." + transport + "." + verdict;
            descriptor.topology = topology.name;
            descriptor.outputType = type.outputType;
            descriptor.transport = transport;
            descriptor.verdict = verdict;
            descriptor.type = &type;
            cases.push_back(descriptor);
        }
    }
    return cases;
}
}
