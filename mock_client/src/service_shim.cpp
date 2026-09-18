#include "service_shim.h"

#include <algorithm>
#include <cctype>

#include "jsonxx.h"

namespace notch_mock
{

namespace cec = ComfyExtensionClient;

namespace
{

// Semver + comparator-range handling for the protocol compatibility gate. Ported
// from the retired client facade so the gate keeps the exact semantics the
// Notch service applies.

struct ParsedSemVer
{
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool valid = false;
};

struct RangeBound
{
    ParsedSemVer version;
    bool inclusive = false;
    bool set = false;
};

struct RangeInterval
{
    RangeBound lower;
    RangeBound upper;
    bool valid = true;
};

std::string Trim(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
    {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool StartsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string ToLower(const std::string& value)
{
    std::string result = value;
    for (size_t i = 0; i < result.size(); ++i)
    {
        result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
    }
    return result;
}

bool ParseIntPart(const std::string& value, int& result)
{
    if (value.empty())
    {
        return false;
    }
    int parsed = 0;
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(value[i])))
        {
            return false;
        }
        parsed = parsed * 10 + (value[i] - '0');
    }
    result = parsed;
    return true;
}

bool ParseSemVer(const std::string& value, ParsedSemVer& version)
{
    version = ParsedSemVer();
    std::string core = value;
    const size_t suffix = core.find_first_of("-+");
    if (suffix != std::string::npos)
    {
        core = core.substr(0, suffix);
    }

    const size_t firstDot = core.find('.');
    const size_t secondDot = firstDot == std::string::npos ? std::string::npos : core.find('.', firstDot + 1);
    if (firstDot == std::string::npos || secondDot == std::string::npos ||
        core.find('.', secondDot + 1) != std::string::npos)
    {
        return false;
    }

    if (!ParseIntPart(core.substr(0, firstDot), version.major) ||
        !ParseIntPart(core.substr(firstDot + 1, secondDot - firstDot - 1), version.minor) ||
        !ParseIntPart(core.substr(secondDot + 1), version.patch))
    {
        return false;
    }
    version.valid = true;
    return true;
}

int CompareSemVer(const ParsedSemVer& first, const ParsedSemVer& second)
{
    if (first.major != second.major)
    {
        return first.major < second.major ? -1 : 1;
    }
    if (first.minor != second.minor)
    {
        return first.minor < second.minor ? -1 : 1;
    }
    if (first.patch != second.patch)
    {
        return first.patch < second.patch ? -1 : 1;
    }
    return 0;
}

RangeBound MaxLowerBound(const RangeBound& first, const RangeBound& second)
{
    if (!first.set)
    {
        return second;
    }
    if (!second.set)
    {
        return first;
    }
    const int comparison = CompareSemVer(first.version, second.version);
    if (comparison > 0)
    {
        return first;
    }
    if (comparison < 0)
    {
        return second;
    }
    RangeBound result = first;
    result.inclusive = first.inclusive && second.inclusive;
    return result;
}

RangeBound MinUpperBound(const RangeBound& first, const RangeBound& second)
{
    if (!first.set)
    {
        return second;
    }
    if (!second.set)
    {
        return first;
    }
    const int comparison = CompareSemVer(first.version, second.version);
    if (comparison < 0)
    {
        return first;
    }
    if (comparison > 0)
    {
        return second;
    }
    RangeBound result = first;
    result.inclusive = first.inclusive && second.inclusive;
    return result;
}

bool ParseRangePart(const std::string& rawPart, RangeInterval& interval)
{
    std::string part = Trim(rawPart);
    if (part.empty())
    {
        return false;
    }

    std::string op = "=";
    if (StartsWith(part, ">=") || StartsWith(part, "<=") || StartsWith(part, "=="))
    {
        op = part.substr(0, 2);
        part = Trim(part.substr(2));
    }
    else if (StartsWith(part, ">") || StartsWith(part, "<") || StartsWith(part, "="))
    {
        op = part.substr(0, 1);
        part = Trim(part.substr(1));
    }

    ParsedSemVer version;
    if (!ParseSemVer(part, version))
    {
        return false;
    }

    RangeBound bound;
    bound.version = version;
    bound.set = true;
    bound.inclusive = op == ">=" || op == "<=" || op == "=" || op == "==";

    if (op == ">=" || op == ">")
    {
        interval.lower = MaxLowerBound(interval.lower, bound);
    }
    else if (op == "<=" || op == "<")
    {
        interval.upper = MinUpperBound(interval.upper, bound);
    }
    else if (op == "=" || op == "==")
    {
        interval.lower = MaxLowerBound(interval.lower, bound);
        interval.upper = MinUpperBound(interval.upper, bound);
    }
    else
    {
        return false;
    }
    return true;
}

RangeInterval ParseProtocolRange(const std::string& value)
{
    RangeInterval interval;
    size_t begin = 0;
    while (begin <= value.size())
    {
        const size_t comma = value.find(',', begin);
        const std::string part =
            comma == std::string::npos ? value.substr(begin) : value.substr(begin, comma - begin);
        if (!ParseRangePart(part, interval))
        {
            interval.valid = false;
            return interval;
        }
        if (comma == std::string::npos)
        {
            break;
        }
        begin = comma + 1;
    }
    return interval;
}

bool ProtocolRangesIntersect(const std::string& first, const std::string& second)
{
    const RangeInterval firstInterval = ParseProtocolRange(first);
    const RangeInterval secondInterval = ParseProtocolRange(second);
    if (!firstInterval.valid || !secondInterval.valid)
    {
        return false;
    }

    const RangeBound lower = MaxLowerBound(firstInterval.lower, secondInterval.lower);
    const RangeBound upper = MinUpperBound(firstInterval.upper, secondInterval.upper);
    if (!lower.set || !upper.set)
    {
        return true;
    }

    const int comparison = CompareSemVer(lower.version, upper.version);
    if (comparison < 0)
    {
        return true;
    }
    if (comparison > 0)
    {
        return false;
    }
    return lower.inclusive && upper.inclusive;
}

// jsonxx field helpers, same shapes the retired facade parsed.

bool ParseJsonObject(const std::string& json, jsonxx::Object& object)
{
    object.reset();
    return object.parse(json);
}

bool GetStringField(const jsonxx::Object& object, const std::string& key, std::string& value)
{
    if (!object.has<jsonxx::String>(key))
    {
        return false;
    }
    value = object.get<jsonxx::String>(key);
    return true;
}

bool GetBoolField(const jsonxx::Object& object, const std::string& key, bool& value)
{
    if (!object.has<jsonxx::Boolean>(key))
    {
        return false;
    }
    value = object.get<jsonxx::Boolean>(key);
    return true;
}

bool GetIntField(const jsonxx::Object& object, const std::string& key, int64_t& value)
{
    if (!object.has<jsonxx::Number>(key))
    {
        return false;
    }
    value = static_cast<int64_t>(object.get<jsonxx::Number>(key));
    return true;
}

bool GetStringArrayField(const jsonxx::Object& object, const std::string& key, std::vector<std::string>& values)
{
    values.clear();
    if (!object.has<jsonxx::Array>(key))
    {
        return false;
    }
    const jsonxx::Array& array = object.get<jsonxx::Array>(key);
    for (size_t i = 0; i < array.size(); ++i)
    {
        if (array.has<jsonxx::String>(static_cast<unsigned int>(i)))
        {
            values.push_back(array.get<jsonxx::String>(static_cast<unsigned int>(i)));
        }
    }
    return true;
}

std::vector<std::string> NormalizeTransportSet(const std::vector<std::string>& values)
{
    std::vector<std::string> normalized;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const std::string value = ToLower(Trim(values[i]));
        if (!value.empty() && std::find(normalized.begin(), normalized.end(), value) == normalized.end())
        {
            normalized.push_back(value);
        }
    }
    return normalized;
}

std::vector<std::string> IntersectTransportSets(
    const std::vector<std::string>& first,
    const std::vector<std::string>& second)
{
    std::vector<std::string> result;
    for (size_t i = 0; i < first.size(); ++i)
    {
        if (std::find(second.begin(), second.end(), first[i]) != second.end())
        {
            result.push_back(first[i]);
        }
    }
    return result;
}

bool StringVectorContains(const std::vector<std::string>& values, const std::string& target)
{
    return std::find(values.begin(), values.end(), target) != values.end();
}

bool IsHttpError(const cec::HttpResponse& response)
{
    return response.status_code < 200 || response.status_code >= 300;
}

cec::Error HttpStatusError(const cec::HttpResponse& response, const std::string& context)
{
    cec::Error error(context);
    error.status_code = response.status_code;
    error.raw_body = response.body;
    if (!response.body.empty())
    {
        error.message += ": ";
        error.message += response.body;
    }
    return error;
}

} // namespace

std::vector<std::string> TransportNames(const std::vector<cec::OutputTransport>& transports)
{
    std::vector<std::string> names;
    for (size_t i = 0; i < transports.size(); ++i)
    {
        names.push_back(cec::OutputTransportName(transports[i]));
    }
    return names;
}

std::vector<cec::OutputTransport> TransportValues(const std::vector<std::string>& names)
{
    std::vector<cec::OutputTransport> transports;
    for (size_t i = 0; i < names.size(); ++i)
    {
        const cec::OutputTransport transport = cec::OutputTransportFromName(names[i]);
        if (transport != cec::OutputTransport::Unset)
        {
            transports.push_back(transport);
        }
    }
    return transports;
}

RequiredFile::RequiredFile()
    : exists(false)
{
}

GeneratedResult::GeneratedResult()
    : status_code(0)
{
}

OutputTransportDecision::OutputTransportDecision()
    : ok(false)
{
}

OutputTransportDecision SelectOutputTransport(const OutputTransportRequest& request)
{
    OutputTransportDecision selection;

    const std::vector<std::string> typeAllowed = NormalizeTransportSet(TransportNames(request.output.transports));
    const std::vector<std::string> serverAvailable = NormalizeTransportSet(TransportNames(request.server.output_transports));
    const std::vector<std::string> clientReachable = NormalizeTransportSet(request.client_reachable_transports);
    const std::vector<std::string> typeAndServer = IntersectTransportSets(typeAllowed, serverAvailable);
    selection.usable_transports = IntersectTransportSets(typeAndServer, clientReachable);

    if (selection.usable_transports.empty())
    {
        selection.error = "No output transport is type-allowed, server-available, and client-reachable";
        return selection;
    }

    const std::string required = ToLower(request.required_transport);
    if (!required.empty())
    {
        if (StringVectorContains(selection.usable_transports, required))
        {
            selection.ok = true;
            selection.transport = required;
            return selection;
        }
        selection.error = "Required output transport '" + required + "' is not usable";
        return selection;
    }

    for (size_t i = 0; i < request.preference_order.size(); ++i)
    {
        const std::string transport = ToLower(request.preference_order[i]);
        if (StringVectorContains(selection.usable_transports, transport))
        {
            selection.ok = true;
            selection.transport = transport;
            return selection;
        }
    }

    selection.ok = true;
    selection.transport = selection.usable_transports[0];
    return selection;
}

ServiceShim::ServiceShim(
    cec::Client& client,
    cec::HttpTransport& http,
    IWebSocketSender* webSocket)
    : m_client(client)
    , m_http(http)
    , m_webSocket(webSocket)
{
}

cec::Result<ServerFacts> ServiceShim::Discover()
{
    return m_client.GetServerFacts();
}

cec::Result<Session> ServiceShim::Connect()
{
    cec::Result<ServerFacts> discovered = Discover();
    if (!discovered)
    {
        return cec::Result<Session>::Fail(discovered.GetError());
    }

    const cec::CppClientFacts clientFacts = cec::Client::GetFacts();
    const ServerFacts& server = discovered.Value();
    if (!ProtocolRangesIntersect(clientFacts.supports_protocol.value, server.supports_protocol.value))
    {
        ParsedSemVer clientVersion;
        ParsedSemVer serverVersion;
        ParseSemVer(clientFacts.protocol_version.value, clientVersion);
        ParseSemVer(server.protocol_version.value, serverVersion);
        if (clientVersion.valid && serverVersion.valid && CompareSemVer(clientVersion, serverVersion) < 0)
        {
            return cec::Result<Session>::Fail(
                "Vendored C++ client protocol range does not support this plugin/server");
        }
        if (clientVersion.valid && serverVersion.valid && CompareSemVer(clientVersion, serverVersion) > 0)
        {
            return cec::Result<Session>::Fail(
                "Plugin/server protocol range does not support this C++ client");
        }
        return cec::Result<Session>::Fail(
            "Plugin/server and C++ client protocol ranges do not intersect");
    }

    if (m_webSocket != NULL)
    {
        cec::Result<bool> announced = SendFeatureFlags();
        if (!announced)
        {
            return cec::Result<Session>::Fail(announced.GetError());
        }
    }

    Session session;
    session.protocol_version = clientFacts.protocol_version;
    session.capabilities = clientFacts.capabilities.Intersect(server.capabilities);
    session.server = server;
    return cec::Result<Session>::Ok(session);
}

cec::Result<bool> ServiceShim::SendFeatureFlags()
{
    if (m_webSocket == NULL)
    {
        return cec::Result<bool>::Fail("ServiceShim has no WebSocket sender");
    }
    std::string error;
    if (!m_webSocket->SendText(m_client.FeatureFlagsMessage(), error))
    {
        return cec::Result<bool>::Fail(error);
    }
    return cec::Result<bool>::Ok(true);
}

cec::Result<std::vector<RequiredFile> > ServiceShim::GetRequiredFiles(const std::string& workflowJson)
{
    jsonxx::Object workflow;
    cec::HttpRequest request;
    request.method = "POST";
    request.path = "/notch/get-required-files";
    request.content_type = "application/json";
    if (ParseJsonObject(workflowJson, workflow))
    {
        request.body = std::string("{\"prompt\":") + workflowJson + "}";
    }
    else
    {
        request.body = "{\"prompt\":{}}";
    }

    cec::HttpResponse response;
    std::string sendError;
    if (!m_http.Send(request, response, sendError))
    {
        return cec::Result<std::vector<RequiredFile> >::Fail(sendError);
    }
    if (IsHttpError(response))
    {
        return cec::Result<std::vector<RequiredFile> >::Fail(
            HttpStatusError(response, "Required files lookup failed"));
    }

    jsonxx::Object root;
    if (!ParseJsonObject(response.body, root))
    {
        return cec::Result<std::vector<RequiredFile> >::Fail("Required files response was not valid JSON");
    }

    std::string serverError;
    if (GetStringField(root, "error", serverError))
    {
        return cec::Result<std::vector<RequiredFile> >::Fail(serverError);
    }
    if (!root.has<jsonxx::Array>("files"))
    {
        return cec::Result<std::vector<RequiredFile> >::Fail(
            "Required files response did not contain a files array");
    }

    std::vector<RequiredFile> files;
    const jsonxx::Array& array = root.get<jsonxx::Array>("files");
    for (size_t i = 0; i < array.size(); ++i)
    {
        if (!array.has<jsonxx::Object>(static_cast<unsigned int>(i)))
        {
            continue;
        }
        const jsonxx::Object& entry = array.get<jsonxx::Object>(static_cast<unsigned int>(i));
        RequiredFile file;
        GetStringField(entry, "node_id", file.node_id);
        GetStringField(entry, "class_type", file.class_type);
        GetStringField(entry, "input_name", file.input_name);
        GetStringField(entry, "category", file.category);
        GetStringField(entry, "filename", file.filename);
        GetStringField(entry, "full_path", file.full_path);
        GetBoolField(entry, "exists", file.exists);
        if (!file.filename.empty())
        {
            files.push_back(file);
        }
    }
    return cec::Result<std::vector<RequiredFile> >::Ok(files);
}

cec::Result<cec::JobHandle> ServiceShim::Submit(const cec::GenerateRequest& request)
{
    // submission itself is codec-owned: Generate builds the inject request,
    // sends it, and parses the response.
    return m_client.Generate(request);
}

cec::Result<GeneratedResult> ServiceShim::FetchOutput(const cec::OutputReady& output) const
{
    if (output.transport != cec::OutputTransport::Http)
    {
        return cec::Result<GeneratedResult>::Fail("FetchOutput only fetches http output artifacts");
    }
    if (output.http.url.empty())
    {
        return cec::Result<GeneratedResult>::Fail("OutputReady.url is required for http output fetch");
    }

    cec::HttpRequest request;
    request.method = "GET";
    request.path = output.http.url;

    cec::HttpResponse response;
    std::string sendError;
    if (!m_http.Send(request, response, sendError))
    {
        return cec::Result<GeneratedResult>::Fail(sendError);
    }
    if (IsHttpError(response))
    {
        return cec::Result<GeneratedResult>::Fail(HttpStatusError(response, "Output fetch failed"));
    }

    GeneratedResult result;
    result.output = output;
    result.status_code = response.status_code;
    result.bytes = response.body;
    return cec::Result<GeneratedResult>::Ok(result);
}

} // namespace notch_mock
