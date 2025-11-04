#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
using namespace std;

namespace kvstore {
static const char* DIRNAME = ".kv015";
static const int N_BUCKETS = 16; // keep total files under limit

static uint64_t fnv1a64(const string &s) {
    const uint64_t FNV_OFFSET = 1469598103934665603ull;
    const uint64_t FNV_PRIME = 1099511628211ull;
    uint64_t h = FNV_OFFSET;
    for (unsigned char c : s) {
        h ^= c;
        h *= FNV_PRIME;
    }
    return h;
}

static string bucket_path(int b) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/bucket_%02d.bin", DIRNAME, b);
    return string(buf);
}

static bool ensure_dir() {
    struct stat st{};
    if (stat(DIRNAME, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(DIRNAME, 0755) == 0;
}

// Binary record format per key:
// [u16 key_len][key bytes][u32 count][count * i32 values sorted asc]

struct Reader {
    FILE* fp;
    explicit Reader(const string& path) { fp = fopen(path.c_str(), "rb"); }
    ~Reader(){ if(fp) fclose(fp);}    
    bool ok() const { return fp != nullptr; }
};

struct Writer {
    FILE* fp;
    explicit Writer(const string& path) { fp = fopen(path.c_str(), "wb"); }
    ~Writer(){ if(fp) fclose(fp);}    
    bool ok() const { return fp != nullptr; }
};

static bool read_u16(FILE* f, uint16_t &x){ return fread(&x, sizeof(x), 1, f) == 1; }
static bool read_u32(FILE* f, uint32_t &x){ return fread(&x, sizeof(x), 1, f) == 1; }
static bool read_i32(FILE* f, int32_t &x){ return fread(&x, sizeof(x), 1, f) == 1; }
static bool write_u16(FILE* f, uint16_t x){ return fwrite(&x, sizeof(x), 1, f) == 1; }
static bool write_u32(FILE* f, uint32_t x){ return fwrite(&x, sizeof(x), 1, f) == 1; }
static bool write_i32(FILE* f, int32_t x){ return fwrite(&x, sizeof(x), 1, f) == 1; }

static void find_values(const string& key, vector<int>& out){
    out.clear();
    if (!ensure_dir()) return;
    uint64_t h = fnv1a64(key);
    int b = (int)(h % N_BUCKETS);
    string path = bucket_path(b);
    Reader r(path);
    if (!r.ok()) return; // no bucket yet
    while (true) {
        uint16_t klen;
        if (!read_u16(r.fp, klen)) break;
        string k; k.resize(klen);
        if (klen && fread(&k[0], 1, klen, r.fp) != klen) break;
        uint32_t cnt;
        if (!read_u32(r.fp, cnt)) break;
        if (k == key) {
            out.resize(cnt);
            for (uint32_t i = 0; i < cnt; ++i) {
                int32_t v; if (!read_i32(r.fp, v)) { out.clear(); return; }
                out[i] = v;
            }
        } else {
            // skip values
            const size_t to_skip = (size_t)cnt * sizeof(int32_t);
            if (fseek(r.fp, (long)to_skip, SEEK_CUR) != 0) { break; }
        }
    }
    // out already sorted in file
}

static void upsert_delete(const string& key, int value, bool is_insert){
    if (!ensure_dir()) return;
    uint64_t h = fnv1a64(key);
    int b = (int)(h % N_BUCKETS);
    string path = bucket_path(b);
    string tmp = path + ".tmp";

    Reader r(path);
    Writer w(tmp);
    if (!w.ok()) return;

    bool found = false;
    vector<int> vals;

    if (r.ok()) {
        while (true) {
            uint16_t klen;
            if (!read_u16(r.fp, klen)) break;
            string k; k.resize(klen);
            if (klen && fread(&k[0], 1, klen, r.fp) != klen) { break; }
            uint32_t cnt;
            if (!read_u32(r.fp, cnt)) break;
            if (k == key) {
                found = true;
                vals.resize(cnt);
                for (uint32_t i = 0; i < cnt; ++i) {
                    int32_t v; if (!read_i32(r.fp, v)) { vals.clear(); cnt = 0; break; }
                    vals[i] = v;
                }
                // modify vals
                if (is_insert) {
                    auto it = lower_bound(vals.begin(), vals.end(), value);
                    if (it == vals.end() || *it != value) vals.insert(it, value);
                } else {
                    auto it = lower_bound(vals.begin(), vals.end(), value);
                    if (it != vals.end() && *it == value) vals.erase(it);
                }
                // write back only if non-empty
                if (!vals.empty()) {
                    uint16_t nk = (uint16_t)key.size();
                    write_u16(w.fp, nk);
                    if (nk) fwrite(key.data(), 1, nk, w.fp);
                    write_u32(w.fp, (uint32_t)vals.size());
                    for (int v : vals) write_i32(w.fp, v);
                }
            } else {
                // passthrough other keys without loading all values at once
                write_u16(w.fp, klen);
                if (klen) fwrite(k.data(), 1, klen, w.fp);
                write_u32(w.fp, cnt);
                // copy cnt ints
                const size_t bytes = (size_t)cnt * sizeof(int32_t);
                size_t remaining = bytes;
                char buf[4096];
                while (remaining > 0) {
                    size_t to_read = min(remaining, (size_t)sizeof(buf));
                    size_t rd = fread(buf, 1, to_read, r.fp);
                    if (rd == 0) { remaining = 0; break; }
                    fwrite(buf, 1, rd, w.fp);
                    remaining -= rd;
                }
            }
        }
    }

    if (!found && is_insert) {
        // append new key
        uint16_t nk = (uint16_t)key.size();
        write_u16(w.fp, nk);
        if (nk) fwrite(key.data(), 1, nk, w.fp);
        write_u32(w.fp, 1u);
        write_i32(w.fp, value);
    }

    // replace original file
    if (w.fp) { fclose(w.fp); w.fp = nullptr; }
    if (r.fp) { fclose(r.fp); r.fp = nullptr; }
    // rename
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        // fallback copy
        FILE* rf = fopen(tmp.c_str(), "rb");
        FILE* wf = fopen(path.c_str(), "wb");
        if (rf && wf) {
            char buf[4096];
            size_t rd;
            while ((rd = fread(buf, 1, sizeof(buf), rf)) > 0) {
                fwrite(buf, 1, rd, wf);
            }
        }
        if (wf) fclose(wf);
        if (rf) fclose(rf);
        std::remove(tmp.c_str());
    }
}

} // namespace kvstore

int main(){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    int n; if(!(cin>>n)) return 0;
    for(int i=0;i<n;++i){
        string cmd; cin>>cmd;
        if(cmd=="insert"){
            string key; int val; cin>>key>>val;
            if (val < 0) continue; // non-negative as per spec
            kvstore::upsert_delete(key, val, true);
        } else if(cmd=="delete"){
            string key; int val; cin>>key>>val;
            if (val < 0) continue;
            kvstore::upsert_delete(key, val, false);
        } else if(cmd=="find"){
            string key; cin>>key;
            vector<int> vals; kvstore::find_values(key, vals);
            if(vals.empty()){
                cout << "null\n";
            } else {
                for(size_t j=0;j<vals.size();++j){
                    if(j) cout << ' ';
                    cout << vals[j];
                }
                cout << '\n';
            }
        } else {
            // ignore unknown
            string rest; getline(cin, rest);
        }
    }
    return 0;
}
