// Scratch probe: dump .af font records (NOT a test). Lists every class tag in
// the doc.dat graph, any blob-valued field over 2 KiB, and the full contents of
// nodes carrying font descriptors.
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine/io/af_layers/container/af_archive.h"
#include "engine/io/af_layers/graph/af_graph.h"
#include "engine/io/af_layers.h"

using namespace pittore::io;
using namespace pittore::io::af_detail;

namespace {

const Graph* G = nullptr;

void dumpTree(const Node* n, int depth);

std::uint32_t typeOf(const Node* n) {
    return (n && !n->types.empty()) ? n->types[0].first : 0;
}

std::vector<std::uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

void dumpValue(const Value& v, int depth) {
    std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
    switch (v.k) {
        case Value::K::Class: dumpTree(v.cls, depth); break;
        case Value::K::Array:
            std::printf("%s[%zu items]\n", pad.c_str(), v.arr.size());
            for (std::size_t i = 0; i < v.arr.size() && i < 4; ++i)
                dumpValue(v.arr[i], depth + 1);
            if (v.arr.size() > 4) std::printf("%s  ...\n", pad.c_str());
            break;
        case Value::K::Str:
            std::printf("%sstr \"%s\"\n", pad.c_str(), v.s.c_str());
            break;
        case Value::K::Blob:
        case Value::K::Struct:
            std::printf("%sblob %zuB\n", pad.c_str(), v.bytes.size());
            for (std::size_t o = 0; o * 16 < v.bytes.size() && o < 3; ++o) {
                std::printf("%s ", pad.c_str());
                for (std::size_t i = o * 16; i < v.bytes.size() && i < (o + 1) * 16; ++i)
                    std::printf("%02x", v.bytes[i]);
                std::printf("\n");
            }
            break;
        case Value::K::VecD:
            std::printf("%svecd[%zu]", pad.c_str(), v.vd.size());
            for (std::size_t i = 0; i < v.vd.size() && i < 8; ++i)
                std::printf(" %.6g", v.vd[i]);
            std::printf("\n");
            break;
        case Value::K::VecF:
            std::printf("%svecf[%zu]", pad.c_str(), v.vf.size());
            for (std::size_t i = 0; i < v.vf.size() && i < 8; ++i)
                std::printf(" %.6g", double(v.vf[i]));
            std::printf("\n");
            break;
        case Value::K::VecI:
            std::printf("%sveci[%zu]", pad.c_str(), v.vi.size());
            for (std::size_t i = 0; i < v.vi.size() && i < 8; ++i)
                std::printf(" %d", v.vi[i]);
            std::printf("\n");
            break;
        case Value::K::F32: std::printf("%sf %.6g\n", pad.c_str(), v.f); break;
        case Value::K::F64: std::printf("%sd %.6g\n", pad.c_str(), v.f); break;
        case Value::K::U8: case Value::K::U16: case Value::K::U32: case Value::K::U64:
        case Value::K::I8: case Value::K::I16: case Value::K::I32: case Value::K::I64:
            std::printf("%sint %llu\n", pad.c_str(), (unsigned long long)v.u);
            break;
        case Value::K::Bool: std::printf("%sbool %d\n", pad.c_str(), (int)v.u); break;
        case Value::K::Enum: case Value::K::Flags:
            std::printf("%senum %llu (ver %u)\n", pad.c_str(), (unsigned long long)v.u, v.ver);
            break;
        case Value::K::Null: std::printf("%snull\n", pad.c_str()); break;
        default: std::printf("%s<kind %d>\n", pad.c_str(), (int)v.k); break;
    }
}

void dumpTree(const Node* n, int depth) {
    if (!n || depth > 10) return;
    std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
    std::printf("%snode %s", pad.c_str(), tagName(typeOf(n)).c_str());
    if (!n->types.empty()) {
        std::printf(" [");
        for (std::size_t i = 0; i < n->types.size(); ++i)
            std::printf("%s%s v%u", i ? ">" : "", tagName(n->types[i].first).c_str(),
                        n->types[i].second);
        std::printf("]");
    }
    std::printf("\n");
    for (const auto& f : n->fields) {
        std::printf("%s  %s:\n", pad.c_str(), tagName(f.first).c_str());
        dumpValue(f.second, depth + 2);
    }
}

void walk(const Node* n, std::map<std::string, int>& classes, std::set<const Node*>& visited) {
    if (!n) return;
    if (!visited.insert(n).second) return;
    for (const auto& t : n->types) ++classes[tagName(t.first)];
    for (const auto& f : n->fields) {
        const Value& v = f.second;
        if (v.k == Value::K::Class) walk(v.cls, classes, visited);
        else if (v.k == Value::K::Array)
            for (const Value& item : v.arr)
                if (item.k == Value::K::Class) walk(item.cls, classes, visited);
    }
}

void walkBlobs(const Node* n, int depth, std::set<const Node*>& visited) {
    if (!n || depth > 14) return;
    if (!visited.insert(n).second) return;
    for (const auto& f : n->fields) {
        const Value& v = f.second;
        if ((v.k == Value::K::Blob || v.k == Value::K::Struct) && v.bytes.size() > 2048) {
            std::printf("BIGBLOB %s node=%s %zuB ", tagName(f.first).c_str(),
                        tagName(typeOf(n)).c_str(), v.bytes.size());
            for (std::size_t i = 0; i < v.bytes.size() && i < 8; ++i)
                std::printf("%02x", v.bytes[i]);
            std::printf("\n");
        }
        if (v.k == Value::K::Array && v.arr.size() > 2048) {
            Value::K k0 = v.arr[0].k;
            std::printf("BIGARR %s node=%s %zuB kind=%d v=", tagName(f.first).c_str(),
                        tagName(typeOf(n)).c_str(), v.arr.size(), (int)k0);
            for (std::size_t i = 0; i < v.arr.size() && i < 8; ++i) {
                if (k0 == Value::K::U8 || k0 == Value::K::I8) std::printf("%02x", (int)v.arr[i].u);
                else std::printf("%u,", (unsigned)v.arr[i].u);
            }
            std::printf("\n");
        }
        if (v.k == Value::K::Class) walkBlobs(v.cls, depth + 1, visited);
        else if (v.k == Value::K::Array)
            for (const Value& item : v.arr)
                if (item.k == Value::K::Class) walkBlobs(item.cls, depth + 1, visited);
    }
}

void walkRecords(const Node* n, std::vector<const Node*>& out, const char* tags[], int count,
                 int depth, std::set<const Node*>& visited) {
    if (!n || depth > 14) return;
    if (!visited.insert(n).second) return;
    for (int i = 0; i < count; ++i) {
        if (typeOf(n) == tag4(tags[i])) { out.push_back(n); break; }
        bool hasField = false;
        for (const auto& f : n->fields)
            if (f.first == tag4(tags[i])) { hasField = true; break; }
        if (hasField) { out.push_back(n); break; }
    }
    for (const auto& f : n->fields) {
        const Value& v = f.second;
        if (v.k == Value::K::Class) walkRecords(v.cls, out, tags, count, depth + 1, visited);
        else if (v.k == Value::K::Array)
            for (const Value& item : v.arr)
                if (item.k == Value::K::Class)
                    walkRecords(item.cls, out, tags, count, depth + 1, visited);
    }
}

std::vector<const Node*> childrenOf(const Node* n, const char* name) {
    std::vector<const Node*> out;
    if (!n) return out;
    for (const auto& f : n->fields) {
        if (f.first != tag4(name)) continue;
        const Value& v = f.second;
        if (v.k == Value::K::Class) out.push_back(v.cls);
        else if (v.k == Value::K::Array)
            for (const Value& item : v.arr)
                if (item.k == Value::K::Class) out.push_back(item.cls);
    }
    return out;
}

const Node* firstOf(const std::vector<const Node*>& v) {
    return v.empty() ? nullptr : v.front();
}

const Node* singleChild(const Node* n, const char* name) {
    auto v = childrenOf(n, name);
    return v.empty() ? nullptr : v.front();
}

void walkRecordsByType(const Node* n, std::vector<const Node*>& out, std::uint32_t tag,
                       std::set<const Node*>& visited, int depth = 0) {
    if (!n || depth > 16) return;
    if (!visited.insert(n).second) return;
    for (const auto& t : n->types)
        if (t.first == tag) { out.push_back(n); break; }
    for (const auto& f : n->fields) {
        const Value& v = f.second;
        if (v.k == Value::K::Class) walkRecordsByType(v.cls, out, tag, visited, depth + 1);
        else if (v.k == Value::K::Array)
            for (const Value& item : v.arr)
                if (item.k == Value::K::Class)
                    walkRecordsByType(item.cls, out, tag, visited, depth + 1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: af_font_probe <file.af>\n");
        return 2;
    }
    const auto file = readFile(argv[1]);
    Archive a = Archive::parse(file);
    const ArchiveEntry* doc = a.head("doc.dat");
    if (!doc) {
        std::printf("no doc.dat entry\n");
        return 1;
    }
    auto bytes = a.extract(*doc);
    Graph g = parseGraph(bytes);
    G = &g;
    std::printf("nodes: %zu\n", g.nodes.size());

    // 1. All class tags, sorted by count.
    std::map<std::string, int> classes;
    std::set<const Node*> visited;
    walk(g.root(), classes, visited);
    for (const auto& c : classes)
        std::printf("class %s x%d\n", c.first.c_str(), c.second);

    // 2. Big blobs anywhere.
    std::printf("--- blobs over 2KiB ---\n");
    walkBlobs(g.root(), 0, visited);

    // 3. Representative dumps of text/font-related classes.
    // The first paragraph's full style runs (alignment, spacing, indent).
    std::vector<const Node*> txt;
    std::set<const Node*> seen2;
    walkRecordsByType(g.root(), txt, tag4("TxtF"), seen2);
    for (const Node* t : txt) {
        std::printf("=== TxtF %s\n", tagName(typeOf(t)).c_str());
        for (const auto& f : t->fields) {
            if (f.first == tag4("StSt")) {
                const Value& v = f.second;
                if (v.k != Value::K::Class) continue;
                for (const Node* b : childrenOf(v.cls, "Blok")) {
                    std::printf("  Blok:\n");
                    for (const char* att : {"PAtt", "GAtt"}) {
                        const Node* a = singleChild(b, att);
                        if (!a) continue;
                        const Node* run = firstOf(childrenOf(a, "Runs"));
                        if (!run) continue;
                        const Node* item = singleChild(run, "Item");
                        if (!item) continue;
                        std::printf("   %s -> Item [%s]:\n", att, tagName(typeOf(item)).c_str());
                        for (const auto& ff : item->fields) {
                            std::printf("    %s:\n", tagName(ff.first).c_str());
                            dumpValue(ff.second, 5);
                        }
                    }
                }
            }
        }
    }

    // 4. The decoded text specs the real walker produces.
    {
        std::string err;
        auto doc = afDecodeLayers(file, &err, ".");
        if (!doc) std::printf("afDecodeLayers failed: %s\n", err.c_str());
        else {
            for (const std::string& ln : doc->log)
                std::printf("LOG %s\n", ln.c_str());
            {
                // Raw TxtF records for both layers: frame box and transform.
                std::vector<const Node*> txt;
                std::set<const Node*> seen2;
                walkRecordsByType(g.root(), txt, tag4("TxtF"), seen2);
                for (const Node* t : txt) {
                    for (const auto& f : t->fields) {
                        if (f.first == tag4("TxtH")) {
                            std::printf("TxtH child:\n");
                            if (f.second.k == Value::K::Class)
                                for (const auto& ff : f.second.cls->fields) {
                                    std::printf("  %s:\n", tagName(ff.first).c_str());
                                    dumpValue(ff.second, 3);
                                }
                        } else if (f.first == tag4("Xfrm")) {
                            std::printf("Xfrm vecd: [");
                            if (f.second.k == Value::K::VecD)
                                for (double d : f.second.vd) std::printf(" %.6g", d);
                            std::printf(" ]\n");
                        } else if (f.first == tag4("ShpB")) {
                            std::printf("ShpB vecd: [");
                            if (f.second.k == Value::K::VecD)
                                for (double d : f.second.vd) std::printf(" %.6g", d);
                            std::printf(" ]\n");
                        }
                    }
                }
            }
            // 4b. Engine-vs-FreeType advance check on the recorded spec values.
            const char* probes[] = {
                "Some people look for the light",
                "Some people look for the light at",
                "Some people look for the light at the end",
                "at the end of the tunnel. But in",
                "at the end of the tunnel. But in August",
                "August you need to look for the castle at",
                "August you need to look for the castle at the",
                "Looking forward to making Disney",
                "Looking forward to making Disney memories;",
                "memories; because you’re never too old for",
                "memories; because you’re never too old for Disney!",
            };
            for (const char* s : probes) {
                pittore::text::TextSpec p;
                p.text = s;
                p.family = "Bakery";
                p.size = 48.0f;
                auto r = pittore::text::rasterize(p);
                if (r)
                    std::printf("ENGINE W \"%s\" = %.1f (lines %d)\n", s, r->layoutWidth,
                                r->lineCount);
                else
                    std::printf("ENGINE W failed\n");
            }
            for (const auto& l : doc->layers) {
                if (!l.isText && !l.hasText) continue;
                std::printf("TEXTSPEC \"%s\" fam=%s size=%.2f wrap=%.1f align=%d "
                            "lead=%.3f gap=%.1f frameH=%.1f origin=(%.1f,%.1f) "
                            "layer=(%d,%d)%ux%u hasText=%d isText=%d\n",
                            l.name.c_str(), l.textSpec.family.c_str(), l.textSpec.size,
                            l.textSpec.wrapWidth, (int)l.textSpec.align,
                            l.textSpec.emLeading, l.textSpec.paragraphGap,
                            l.textFrameHeight, l.textOriginX, l.textOriginY,
                            l.left, l.top, l.width, l.height, l.hasText ? 1 : 0,
                            l.isText ? 1 : 0);
                if (l.isText && l.hasText) std::printf("  spec.text=\"%s\"\n",
                                                       l.textSpec.text.c_str());
            }
        }
    }
    return 0;
}
