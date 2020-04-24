#ifndef ARBITER_IS_AMALGAMATION
#include <arbiter/arbiter.hpp>

#include <arbiter/driver.hpp>
#include <arbiter/util/sha256.hpp>
#include <arbiter/util/json.hpp>
#include <arbiter/util/transforms.hpp>
#include <arbiter/util/util.hpp>
#endif

#include <algorithm>
#include <cstdlib>
#include <sstream>

#ifdef ARBITER_CUSTOM_NAMESPACE
namespace ARBITER_CUSTOM_NAMESPACE
{
#endif

namespace arbiter
{

namespace
{
    const std::string delimiter("://");

#ifdef ARBITER_CURL
    const std::size_t concurrentHttpReqs(32);
    const std::size_t httpRetryCount(8);
#endif

    json getConfig(const std::string& s)
    {
        json in(s.size() ? json::parse(s) : json::object());

        json config;
        std::string path("~/.arbiter/config.json");
        std::cout << "path is " << path << std::endl;

        if      (auto p = env("ARBITER_CONFIG_FILE")) path = *p;
        else if (auto p = env("ARBITER_CONFIG_PATH")) path = *p;

        if (auto data = drivers::Fs().tryGet(path)) config = json::parse(*data);

        if (in.is_null()) in = json::object();
        if (config.is_null()) config = json::object();

        return merge(in, config);
    }
}

Arbiter::Arbiter() : Arbiter(json().dump()) { }

Arbiter::Arbiter(const std::string s)
    : m_drivers()
#ifdef ARBITER_CURL
    , m_pool(
            new http::Pool(
                concurrentHttpReqs,
                httpRetryCount,
                getConfig(s).dump()))
#endif
{
    using namespace drivers;

    const json c(getConfig(s));

    if (auto d = Fs::create())
    {
        m_drivers[d->type()] = std::move(d);
    }

    if (auto d = Test::create())
    {
        m_drivers[d->type()] = std::move(d);
    }

#ifdef ARBITER_CURL
    if (auto d = Http::create(*m_pool))
    {
        m_drivers[d->type()] = std::move(d);
    }

    if (auto d = Https::create(*m_pool))
    {
        m_drivers[d->type()] = std::move(d);
    }

    {
        auto dlist(S3::create(*m_pool, c.value("s3", json()).dump()));
        for (auto& d : dlist) m_drivers[d->type()] = std::move(d);
    }

    // Credential-based drivers should probably all do something similar to the
    // S3 driver to support multiple profiles.
    if (auto d = Dropbox::create(*m_pool, c.value("dropbox", json()).dump()))
    {
        m_drivers[d->type()] = std::move(d);
    }

#ifdef ARBITER_OPENSSL
    if (auto d = Google::create(*m_pool, c.value("gs", json()).dump()))
    {
        m_drivers[d->type()] = std::move(d);
    }
#endif

#endif
}

bool Arbiter::hasDriver(const std::string path) const
{
    return m_drivers.count(getProtocol(path));
}

void Arbiter::addDriver(const std::string type, std::unique_ptr<Driver> driver)
{
    if (!driver) throw ArbiterError("Cannot add empty driver for " + type);
    m_drivers[type] = std::move(driver);
}

std::string Arbiter::get(const std::string path) const
{
    return getDriver(path).get(stripProtocol(path));
}

std::vector<char> Arbiter::getBinary(const std::string path) const
{
    return getDriver(path).getBinary(stripProtocol(path));
}

std::unique_ptr<std::string> Arbiter::tryGet(std::string path) const
{
    return getDriver(path).tryGet(stripProtocol(path));
}

std::unique_ptr<std::vector<char>> Arbiter::tryGetBinary(std::string path) const
{
    return getDriver(path).tryGetBinary(stripProtocol(path));
}

std::size_t Arbiter::getSize(const std::string path) const
{
    return getDriver(path).getSize(stripProtocol(path));
}

std::unique_ptr<std::size_t> Arbiter::tryGetSize(const std::string path) const
{
    return getDriver(path).tryGetSize(stripProtocol(path));
}

void Arbiter::put(const std::string path, const std::string& data) const
{
    return getDriver(path).put(stripProtocol(path), data);
}

void Arbiter::put(const std::string path, const std::vector<char>& data) const
{
    return getDriver(path).put(stripProtocol(path), data);
}

std::string Arbiter::get(
        const std::string path,
        const http::Headers headers,
        const http::Query query) const
{
    return getHttpDriver(path).get(stripProtocol(path), headers, query);
}

std::unique_ptr<std::string> Arbiter::tryGet(
        const std::string path,
        const http::Headers headers,
        const http::Query query) const
{
    return getHttpDriver(path).tryGet(stripProtocol(path), headers, query);
}

std::vector<char> Arbiter::getBinary(
        const std::string path,
        const http::Headers headers,
        const http::Query query) const
{
    return getHttpDriver(path).getBinary(stripProtocol(path), headers, query);
}

std::unique_ptr<std::vector<char>> Arbiter::tryGetBinary(
        const std::string path,
        const http::Headers headers,
        const http::Query query) const
{
    return getHttpDriver(path).tryGetBinary(stripProtocol(path), headers, query);
}

void Arbiter::put(
        const std::string path,
        const std::string& data,
        const http::Headers headers,
        const http::Query query) const
{
    return getHttpDriver(path).put(stripProtocol(path), data, headers, query);
}

void Arbiter::put(
        const std::string path,
        const std::vector<char>& data,
        const http::Headers headers,
        const http::Query query) const
{
    return getHttpDriver(path).put(stripProtocol(path), data, headers, query);
}

void Arbiter::copy(
        const std::string src,
        const std::string dst,
        const bool verbose) const
{
    if (src.empty()) throw ArbiterError("Cannot copy from empty source");
    if (dst.empty()) throw ArbiterError("Cannot copy to empty destination");

    // Globify the source path if it's a directory.  In this case, the source
    // already ends with a slash.
    const std::string srcToResolve(src + (isDirectory(src) ? "**" : ""));

    if (srcToResolve.back() != '*')
    {
        // The source is a single file.
        copyFile(src, dst, verbose);
    }
    else
    {
        // We'll need this to mirror the directory structure in the output.
        // All resolved paths will contain this common prefix, so we can
        // determine any nested paths from recursive resolutions by stripping
        // that common portion.
        const Endpoint& srcEndpoint(getEndpoint(stripPostfixing(src)));
        const std::string commonPrefix(srcEndpoint.prefixedRoot());

        const Endpoint dstEndpoint(getEndpoint(dst));

        if (srcEndpoint.prefixedRoot() == dstEndpoint.prefixedRoot())
        {
            throw ArbiterError("Cannot copy directory to itself");
        }

        int i(0);
        const auto paths(resolve(srcToResolve, verbose));

        for (const auto& path : paths)
        {
            const std::string subpath(path.substr(commonPrefix.size()));

            if (verbose)
            {
                std::cout <<
                    ++i << " / " << paths.size() << ": " <<
                    path << " -> " << dstEndpoint.prefixedFullPath(subpath) <<
                    std::endl;
            }

            if (dstEndpoint.isLocal())
            {
                mkdirp(getDirname(dstEndpoint.fullPath(subpath)));
            }

            dstEndpoint.put(subpath, getBinary(path));
        }
    }
}

void Arbiter::copyFile(
        const std::string file,
        std::string dst,
        const bool verbose) const
{
    if (dst.empty()) throw ArbiterError("Cannot copy to empty destination");

    const Endpoint dstEndpoint(getEndpoint(dst));

    if (isDirectory(dst))
    {
        // If the destination is a directory, maintain the basename of the
        // source file.
        dst += getBasename(file);
    }

    if (verbose) std::cout << file << " -> " << dst << std::endl;

    if (dstEndpoint.isLocal()) mkdirp(getDirname(dst));

    if (getEndpoint(file).type() == dstEndpoint.type())
    {
        // If this copy is within the same driver domain, defer to the
        // hopefully specialized copy method.
        getDriver(file).copy(stripProtocol(file), stripProtocol(dst));
    }
    else
    {
        // Otherwise do a GET/PUT for the copy.
        put(dst, getBinary(file));
    }
}

bool Arbiter::isRemote(const std::string path) const
{
    return getDriver(path).isRemote();
}

bool Arbiter::isLocal(const std::string path) const
{
    return !isRemote(path);
}

bool Arbiter::exists(const std::string path) const
{
    return tryGetSize(path).get() != nullptr;
}

bool Arbiter::isHttpDerived(const std::string path) const
{
    return tryGetHttpDriver(path) != nullptr;
}

std::vector<std::string> Arbiter::resolve(
        const std::string path,
        const bool verbose) const
{
    return getDriver(path).resolve(stripProtocol(path), verbose);
}

Endpoint Arbiter::getEndpoint(const std::string root) const
{
    return Endpoint(getDriver(root), stripProtocol(root));
}

const Driver& Arbiter::getDriver(const std::string path) const
{
    const auto type(getProtocol(path));

    if (!m_drivers.count(type))
    {
        throw ArbiterError("No driver for " + path);
    }

    return *m_drivers.at(type);
}

const drivers::Http* Arbiter::tryGetHttpDriver(const std::string path) const
{
    return dynamic_cast<const drivers::Http*>(&getDriver(path));
}

const drivers::Http& Arbiter::getHttpDriver(const std::string path) const
{
    if (auto d = tryGetHttpDriver(path)) return *d;
    else throw ArbiterError("Cannot get driver for " + path + " as HTTP");
}

LocalHandle Arbiter::getLocalHandle(
        const std::string path,
        const Endpoint& tempEndpoint) const
{
    const Endpoint fromEndpoint(getEndpoint(getDirname(path)));
    return fromEndpoint.getLocalHandle(getBasename(path));
}

LocalHandle Arbiter::getLocalHandle(
        const std::string path,
        std::string tempPath) const
{
    if (tempPath.empty()) tempPath = getTempPath();
    return getLocalHandle(path, getEndpoint(tempPath));
}

} // namespace arbiter

#ifdef ARBITER_CUSTOM_NAMESPACE
}
#endif

