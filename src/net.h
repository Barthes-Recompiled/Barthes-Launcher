// net.h - the little the launcher needs from the internet: GitLab's releases API, downloading a release, checking it
// (net.cpp, WinHTTP + BCrypt, both part of Windows)
#pragma once
#include <windows.h>

#include <string>
#include <vector>

struct Release {
    std::string tag;          // "v0.4.0"
    std::string version;      // "0.4.0"
    std::string name;
    std::string zip_url;      // the first asset link ending in .zip
    std::string zip_name;
    std::string sha256_url;   // <zip_name>.sha256, when the release has one
};

struct Transfer {
    volatile LONG64 done = 0;
    volatile LONG64 total = 0;  // 0 while the size is unknown
    volatile LONG cancel = 0;
};

// GET url into body. FALSE with error filled on a network failure or a status other than 200.
bool net_Get(const std::wstring& url, std::string& body, std::string& error);

// GET url into the file at path (written as path + ".part", renamed once whole)
bool net_Download(const std::wstring& url, const std::wstring& path, Transfer* t, std::string& error);

// The newest release of a GitLab project ("barthes_recompiled/hp1_recomp"). FALSE with error empty when the
// project simply has no release yet.
bool net_LatestRelease(const std::string& project, Release& r, std::string& error);

// lowercase hex SHA-256 of a file, empty when it could not be read
std::string net_Sha256File(const std::wstring& path, Transfer* t);

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& w);
