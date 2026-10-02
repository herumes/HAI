// Command line front end for buildscript.cpp.
//
//   bs_cli <script-or-reply.txt> [--names block_ids.txt] [--out prefab.json]
//          [--layers] [--materials] [--fix] [--max blocks]
//
// Reads a build script (or a whole AI reply with the script somewhere in it),
// expands it, and prints what the Build tab's "make a prefab" button would show.
// --out writes the prefab; --layers prints an ASCII map of every layer so a build
// can be checked without the game; --fix prints the message to send the AI back.
#include "pch.h"
#include "buildscript.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

static std::string ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    std::string in, names, out;
    bool layers = false, materials = false, fix = false;
    size_t maxBlocks = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--names" && i + 1 < argc) names = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--max" && i + 1 < argc) maxBlocks = (size_t)std::atoll(argv[++i]);
        else if (a == "--layers") layers = true;
        else if (a == "--materials") materials = true;
        else if (a == "--fix") fix = true;
        else in = a;
    }
    if (in.empty()) { std::printf("usage: bs_cli <script> [--names block_ids.txt] [--out prefab.json] [--layers] [--materials] [--fix] [--max blocks]\n"); return 2; }

    BuildScript::NameBook book;
    if (!names.empty()) {
        std::ifstream f(names);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            book.Add(line.substr(tab + 1));
        }
    }

    BuildScript::Result r;
    BuildScript::Options opt;
    if (maxBlocks) opt.maxBlocks = maxBlocks;
    BuildScript::Expand(ReadAll(in), book, opt, r);

    std::printf("%s\n", BuildScript::Summary(r).substr(0, materials ? std::string::npos : BuildScript::Summary(r).find('\n')).c_str());
    for (const std::string& e : r.errors) std::printf("ERROR: %s\n", e.c_str());
    for (const auto& u : r.unknown) {
        std::printf("UNKNOWN: %s x%zu ->", u.name.c_str(), u.count);
        for (const std::string& s : u.suggestions) std::printf(" %s", s.c_str());
        std::printf("\n");
    }
    for (const std::string& w : r.warnings) std::printf("warning: %s\n", w.c_str());
    if (fix && !r.Ok()) std::printf("\n--- message for the AI ---\n%s", BuildScript::FixRequest(r).c_str());

    if (layers && !r.blocks.empty()) {
        // One glyph per material, in order of use.
        std::map<std::string, char> glyph;
        const std::string pool = "#@%&*+=ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        for (const auto& m : r.materials) {
            const size_t k = glyph.size();
            glyph[m.first] = k < pool.size() ? pool[k] : '?';
        }
        std::printf("\nlegend (rows are z north->south, columns are x west->east):\n");
        for (const auto& m : r.materials) std::printf("  %c %s\n", glyph[m.first], m.first.c_str());
        std::map<std::tuple<int, int, int>, const BuildScript::Block*> at;
        for (const auto& b : r.blocks) at[{ b.x, b.y, b.z }] = &b;
        for (int y = r.minY; y <= r.maxY; ++y) {
            std::printf("\n-- y = %d --\n", y);
            for (int z = r.minZ; z <= r.maxZ; ++z) {
                std::string row;
                for (int x = r.minX; x <= r.maxX; ++x) {
                    auto it = at.find({ x, y, z });
                    row += it == at.end() ? '.' : glyph[it->second->name];
                }
                std::printf("%s\n", row.c_str());
            }
        }
    }

    if (!out.empty() && !r.blocks.empty()) {
        std::ofstream f(out, std::ios::binary);
        f << BuildScript::ToPrefabJson(r);
        std::printf("\nwrote %s\n", out.c_str());
    }
    return r.errors.empty() ? 0 : 1;
}
