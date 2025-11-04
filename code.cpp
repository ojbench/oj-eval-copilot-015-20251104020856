#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
using namespace std;

namespace kvstore {
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
    snprintf(buf, sizeof(buf), "bucket_%02d.bin", b);
    return string(buf);
}

// Append-only record format per operation:
// [u16 key_len][key bytes][i32 value][u8 op] where op: 1=insert, 2=delete

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
static bool read_u8(FILE* f, uint8_t &x){ return fread(&x, sizeof(x), 1, f) == 1; }
static bool write_u8(FILE* f, uint8_t x){ return fwrite(&x, sizeof(x), 1, f) == 1; }

static void find_values(const string& key, vector<int>& out){
    out.clear();
    uint64_t h = fnv1a64(key);
    int b = (int)(h % N_BUCKETS);
    string path = bucket_path(b);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    vector<pair<int,int8_t>> ops; ops.reserve(64);
    while (true) {
        uint16_t klen; if (!read_u16(f, klen)) break;
        string k; k.resize(klen);
        if (klen && fread(&k[0], 1, klen, f) != klen) break;
        int32_t v; if (!read_i32(f, v)) break;
        uint8_t op; if (!read_u8(f, op)) break;
        if (k == key) ops.emplace_back((int)v, (int8_t)op);
    }
    fclose(f);
    if (ops.empty()) return;
    stable_sort(ops.begin(), ops.end(), [](const pair<int,int8_t>& a, const pair<int,int8_t>& b){
        return a.first < b.first;
    });
    out.clear(); out.reserve(ops.size());
    for (size_t i = 0; i < ops.size(); ) {
        size_t j = i; while (j < ops.size() && ops[j].first == ops[i].first) ++j;
        int8_t lastop = ops[j-1].second; // stable_sort preserves input order among equals
        if (lastop == 1) out.push_back(ops[i].first);
        i = j;
    }
    // out is sorted ascending by value
}

static void upsert_delete(const string& key, int value, bool is_insert){
    uint64_t h = fnv1a64(key);
    int b = (int)(h % N_BUCKETS);
    string path = bucket_path(b);
    FILE* f = fopen(path.c_str(), "ab");
    if (!f) return;
    uint16_t nk = (uint16_t)key.size();
    write_u16(f, nk);
    if (nk) fwrite(key.data(), 1, nk, f);
    write_i32(f, (int32_t)value);
    write_u8(f, is_insert ? (uint8_t)1 : (uint8_t)2);
    fclose(f);
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
