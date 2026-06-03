#ifndef NOTCH_MOCK_DELIVERY_TYPES_H
#define NOTCH_MOCK_DELIVERY_TYPES_H

#include <string>
#include <vector>

#include "matrix.h"  // VerificationClass, Phase

namespace notch_mock
{
// One Notch output type and everything the delivery generator needs to inject a
// fixture, wire the NotchOutputNode slot, and verify the delivered artifact.
struct DeliveryTypeContract
{
    std::string outputType;    // "image" | "audio" | ... | "file_path"
    std::string inputType;     // NotchSingleInput type, e.g. "IMAGE", "FILE_3D", "STRING"
    std::string slotName;      // NotchOutputNode slot, e.g. "image", "file_path"
    std::string fixtureFile;   // basename under the asset root, or "" for inline/raw-buffer
    bool isImage;              // true only for "image" (the cuda-allowed type)
    VerificationClass verify;  // verifier for the disk/http delivered artifact
};

// The 7 Notch output types (core/data_types.py _NOTCH_OUTPUT_TYPES), with the
// verification class confirmed from the ComfyUI-Notch delivery source.
const std::vector<DeliveryTypeContract>& DeliveryTypes();

// type-allowed transports: image -> {cuda,disk,http}; everything else -> {disk,http}.
std::vector<std::string> TypeAllowedTransports(const DeliveryTypeContract& type);

// Realizable client-reachable set per currently selected live topology. The
// route-disk reachable set is exercised per case because delivery_remote runs
// both http-only rejects and named-route disk positives in one job.
std::vector<std::string> TopologyReachable(Phase phase);

// usable = type-allowed n server-available n client-reachable.
std::vector<std::string> UsableTransports(const DeliveryTypeContract& type,
                                          const std::vector<std::string>& serverAvailable,
                                          const std::vector<std::string>& clientReachable);

// One delivery topology: its server-available and client-reachable transport sets.
struct TopologyConfig
{
    std::string name;                          // "local" | "remote-http" | "remote-route-disk"
    std::vector<std::string> serverAvailable;  // from /features (GPU runner -> {cuda,disk,http})
    std::vector<std::string> clientReachable;  // local{c,d,h} http-only{h} route-disk{d,h}
};

// One generated delivery case, derived purely from the (topology x type x
// transport) arithmetic. The id is self-describing and the verdict is the
// expectation; both come from the same classification so they cannot disagree.
struct DeliveryCaseDescriptor
{
    std::string id;          // "<topology>.<type>.<transport>.<verdict>"
    std::string topology;
    std::string outputType;
    std::string transport;
    std::string verdict;     // "deliver" | "reject-type" | "reject-server" | "reject-client"
    const DeliveryTypeContract* type = nullptr;  // points into DeliveryTypes()
};

// Generate every case for one topology by classifying each (type x transport)
// triple over {cuda,disk,http}: usable -> deliver; not type-allowed -> reject-type
// (emitted only for the "local" topology, since type exclusion is
// topology-independent and proved once); not server-available -> reject-server;
// not client-reachable -> reject-client. This is the spec's "the arithmetic is
// the case set": local 21, remote-http 15, remote-route-disk 15.
std::vector<DeliveryCaseDescriptor> GenerateDeliveryCases(const TopologyConfig& topology);
}

#endif
