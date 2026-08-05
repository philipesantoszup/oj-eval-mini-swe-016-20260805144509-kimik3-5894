#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <climits>
#include <string>
#include <unordered_map>
#include <vector>
using namespace std;

typedef int64_t i64;

#define BLOCK_SIZE 4096
#define LEAF_MAX 59
#define LEAF_MIN 30
#define IN_MAX_CHILD 53
#define IN_MAX_KEYS 52
#define IN_MIN_KEYS 26
#define CACHE_CAP 8192

struct Key {
    char str[64];
    int value;
};

static inline int keyCmp(const Key& a, const Key& b) {
    int c = memcmp(a.str, b.str, 64);
    if (c) return c;
    if (a.value < b.value) return -1;
    if (a.value > b.value) return 1;
    return 0;
}

struct Node {
    int isLeaf;
    int keyCount;
    i64 next; // leaf: next leaf block id (-1 none); unused for internal
    union {
        Key leafKeys[LEAF_MAX + 1];         // 60 * 68 = 4080
        struct {
            i64 children[IN_MAX_CHILD + 1]; // 54 * 8  = 432
            Key keys[IN_MAX_KEYS + 1];      // 53 * 68 = 3604
        } in;                               // total 4036
    };
};
static_assert(sizeof(Node) <= BLOCK_SIZE, "Node too large");

struct Meta {
    i64 root;      // block id of root, -1 when tree is empty
    i64 nBlocks;   // number of blocks ever allocated (file size / BLOCK_SIZE)
    i64 freeHead;  // head of free block list, -1 none
    i64 entryCount;
};

class Storage {
public:
    FILE* f;
    Meta meta;
private:
    struct Slot {
        i64 block;
        int dirty;
        int prev, next;
        char data[BLOCK_SIZE];
    };
    Slot* slots;
    unordered_map<i64, int> mp;
    vector<int> freeSlots;
    int head, tail; // LRU list: head = most recently used

    void fileRead(i64 block, char* out) {
        fseek(f, block * (i64)BLOCK_SIZE, SEEK_SET);
        size_t r = fread(out, 1, BLOCK_SIZE, f);
        if (r < BLOCK_SIZE) memset(out + r, 0, BLOCK_SIZE - r);
    }
    void fileWrite(i64 block, const char* data) {
        fseek(f, block * (i64)BLOCK_SIZE, SEEK_SET);
        fwrite(data, 1, BLOCK_SIZE, f);
    }
    void detach(int s) {
        if (slots[s].prev >= 0) slots[slots[s].prev].next = slots[s].next;
        else head = slots[s].next;
        if (slots[s].next >= 0) slots[slots[s].next].prev = slots[s].prev;
        else tail = slots[s].prev;
        slots[s].prev = slots[s].next = -1;
    }
    void pushFront(int s) {
        slots[s].prev = -1;
        slots[s].next = head;
        if (head >= 0) slots[head].prev = s;
        head = s;
        if (tail < 0) tail = s;
    }
public:
    Storage(const char* path) : head(-1), tail(-1) {
        f = fopen(path, "r+b");
        if (!f) {
            f = fopen(path, "w+b");
            if (!f) { fprintf(stderr, "cannot open data file\n"); exit(1); }
            meta.root = -1; meta.nBlocks = 1; meta.freeHead = -1; meta.entryCount = 0;
            char zero[BLOCK_SIZE];
            memset(zero, 0, BLOCK_SIZE);
            memcpy(zero, &meta, sizeof(Meta));
            fseek(f, 0, SEEK_SET);
            fwrite(zero, 1, BLOCK_SIZE, f);
            fflush(f);
        } else {
            char buf[BLOCK_SIZE];
            fseek(f, 0, SEEK_SET);
            size_t r = fread(buf, 1, BLOCK_SIZE, f);
            if (r < sizeof(Meta)) {
                meta.root = -1; meta.nBlocks = 1; meta.freeHead = -1; meta.entryCount = 0;
            } else {
                memcpy(&meta, buf, sizeof(Meta));
            }
        }
        slots = (Slot*)malloc(sizeof(Slot) * CACHE_CAP);
        if (!slots) { fprintf(stderr, "out of memory\n"); exit(1); }
        freeSlots.reserve(CACHE_CAP);
        for (int i = CACHE_CAP - 1; i >= 0; i--) {
            slots[i].block = -1; slots[i].dirty = 0; slots[i].prev = slots[i].next = -1;
            freeSlots.push_back(i);
        }
        mp.reserve(CACHE_CAP + CACHE_CAP / 2);
    }
    ~Storage() {
        flush();
        if (f) fclose(f);
        free(slots);
    }
    // Returns pointer to block data in cache. The pointer is only valid until
    // the next get()/alloc()/freeBlock() call.
    char* get(i64 block, bool forWrite) {
        auto it = mp.find(block);
        if (it != mp.end()) {
            int s = it->second;
            if (s != head) { detach(s); pushFront(s); }
            if (forWrite) slots[s].dirty = 1;
            return slots[s].data;
        }
        int s;
        if (!freeSlots.empty()) {
            s = freeSlots.back(); freeSlots.pop_back();
        } else {
            s = tail; // evict least recently used
            detach(s);
            mp.erase(slots[s].block);
            if (slots[s].dirty) fileWrite(slots[s].block, slots[s].data);
        }
        slots[s].block = block;
        slots[s].dirty = forWrite ? 1 : 0;
        fileRead(block, slots[s].data);
        mp[block] = s;
        pushFront(s);
        return slots[s].data;
    }
    i64 alloc() {
        if (meta.freeHead != -1) {
            i64 id = meta.freeHead;
            char* d = get(id, true);
            i64 nxt;
            memcpy(&nxt, d, sizeof(i64));
            meta.freeHead = nxt;
            return id;
        }
        return meta.nBlocks++;
    }
    void freeBlock(i64 id) {
        char* d = get(id, true);
        memcpy(d, &meta.freeHead, sizeof(i64));
        meta.freeHead = id;
    }
    void flush() {
        for (int i = 0; i < CACHE_CAP; i++) {
            if (slots[i].block >= 0 && slots[i].dirty) {
                fileWrite(slots[i].block, slots[i].data);
                slots[i].dirty = 0;
            }
        }
        char buf[BLOCK_SIZE];
        memset(buf, 0, BLOCK_SIZE);
        memcpy(buf, &meta, sizeof(Meta));
        fseek(f, 0, SEEK_SET);
        fwrite(buf, 1, BLOCK_SIZE, f);
        fflush(f);
    }
};

class BPT {
public:
    Storage st;
    BPT(const char* path) : st(path) {}

    static inline int leafLower(const Node& n, const Key& key) {
        int lo = 0, hi = n.keyCount;
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (keyCmp(n.leafKeys[mid], key) < 0) lo = mid + 1; else hi = mid;
        }
        return lo;
    }
    // child index to descend: number of separator keys <= key
    static inline int inChild(const Node& n, const Key& key) {
        int lo = 0, hi = n.keyCount;
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (keyCmp(n.in.keys[mid], key) <= 0) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    void insert(const Key& key) {
        if (st.meta.root == -1) {
            i64 id = st.alloc();
            Node* r = (Node*)st.get(id, true);
            r->isLeaf = 1; r->keyCount = 1; r->next = -1;
            r->leafKeys[0] = key;
            st.meta.root = id;
            st.meta.entryCount++;
            return;
        }
        i64 path[40]; int cidx[40]; int depth = 0;
        i64 cur = st.meta.root;
        while (true) {
            Node* n = (Node*)st.get(cur, false);
            if (n->isLeaf) break;
            int ci = inChild(*n, key);
            path[depth] = cur; cidx[depth] = ci; depth++;
            cur = n->in.children[ci];
        }
        i64 leafId = cur;
        {
            Node* leaf = (Node*)st.get(leafId, true);
            int pos = leafLower(*leaf, key);
            if (pos < leaf->keyCount && keyCmp(leaf->leafKeys[pos], key) == 0) return; // duplicate
            memmove(&leaf->leafKeys[pos + 1], &leaf->leafKeys[pos], (leaf->keyCount - pos) * sizeof(Key));
            leaf->leafKeys[pos] = key;
            leaf->keyCount++;
            if (leaf->keyCount <= LEAF_MAX) { st.meta.entryCount++; return; }
        }
        st.meta.entryCount++;
        // split leaf (work on a stack copy to stay safe across cache ops)
        Node leafC;
        memcpy(&leafC, st.get(leafId, true), sizeof(Node));
        i64 newId = st.alloc();
        Node* nl = (Node*)st.get(newId, true);
        int total = leafC.keyCount; // LEAF_MAX + 1 = 60
        int leftCount = total / 2;  // 30
        int rightCount = total - leftCount; // 30
        nl->isLeaf = 1;
        nl->keyCount = rightCount;
        memcpy(nl->leafKeys, &leafC.leafKeys[leftCount], rightCount * sizeof(Key));
        nl->next = leafC.next;
        leafC.next = newId;
        leafC.keyCount = leftCount;
        Key pushKey = nl->leafKeys[0];
        memcpy(st.get(leafId, true), &leafC, sizeof(Node));
        // propagate the split upwards
        i64 rightChild = newId;
        int d = depth - 1;
        while (d >= 0) {
            i64 pid = path[d];
            int ci = cidx[d];
            Node p;
            memcpy(&p, st.get(pid, true), sizeof(Node));
            memmove(&p.in.keys[ci + 1], &p.in.keys[ci], (p.keyCount - ci) * sizeof(Key));
            p.in.keys[ci] = pushKey;
            memmove(&p.in.children[ci + 2], &p.in.children[ci + 1], (p.keyCount - ci) * sizeof(i64));
            p.in.children[ci + 1] = rightChild;
            p.keyCount++;
            if (p.keyCount <= IN_MAX_KEYS) {
                memcpy(st.get(pid, true), &p, sizeof(Node));
                return;
            }
            // split internal node: keyCount = 53 keys, 54 children
            int mid = p.keyCount / 2; // 26
            Key upKey = p.in.keys[mid];
            i64 newPid = st.alloc();
            Node* np = (Node*)st.get(newPid, true);
            np->isLeaf = 0;
            int rightKeys = p.keyCount - mid - 1; // 26
            np->keyCount = rightKeys;
            memcpy(np->in.keys, &p.in.keys[mid + 1], rightKeys * sizeof(Key));
            memcpy(np->in.children, &p.in.children[mid + 1], (rightKeys + 1) * sizeof(i64));
            p.keyCount = mid;
            memcpy(st.get(pid, true), &p, sizeof(Node));
            pushKey = upKey;
            rightChild = newPid;
            d--;
        }
        // create new root
        i64 rid = st.alloc();
        Node* r = (Node*)st.get(rid, true);
        r->isLeaf = 0;
        r->keyCount = 1;
        r->in.keys[0] = pushKey;
        r->in.children[0] = st.meta.root;
        r->in.children[1] = rightChild;
        st.meta.root = rid;
    }

    void erase(const Key& key) {
        if (st.meta.root == -1) return;
        i64 path[40]; int cidx[40]; int depth = 0;
        i64 cur = st.meta.root;
        while (true) {
            Node* n = (Node*)st.get(cur, false);
            if (n->isLeaf) break;
            int ci = inChild(*n, key);
            path[depth] = cur; cidx[depth] = ci; depth++;
            cur = n->in.children[ci];
        }
        i64 leafId = cur;
        {
            Node* leaf = (Node*)st.get(leafId, true);
            int pos = leafLower(*leaf, key);
            if (pos >= leaf->keyCount || keyCmp(leaf->leafKeys[pos], key) != 0) return; // not found
            memmove(&leaf->leafKeys[pos], &leaf->leafKeys[pos + 1], (leaf->keyCount - pos - 1) * sizeof(Key));
            leaf->keyCount--;
        }
        st.meta.entryCount--;
        if (depth == 0) {
            // root is a leaf
            Node* leaf = (Node*)st.get(leafId, false);
            if (leaf->keyCount == 0) {
                st.freeBlock(leafId);
                st.meta.root = -1;
            }
            return;
        }
        {
            Node* leaf = (Node*)st.get(leafId, false);
            if (leaf->keyCount >= LEAF_MIN) return;
        }
        // fix underflow, walking up
        int d = depth - 1;
        i64 nodeId = leafId;
        bool nodeIsLeaf = true;
        while (true) {
            i64 pid = path[d];
            int ci = cidx[d];
            Node p;
            memcpy(&p, st.get(pid, false), sizeof(Node));
            i64 leftId = (ci > 0) ? p.in.children[ci - 1] : -1;
            i64 rightId = (ci < p.keyCount) ? p.in.children[ci + 1] : -1;
            if (nodeIsLeaf) {
                Node node;
                memcpy(&node, st.get(nodeId, false), sizeof(Node));
                if (leftId != -1) {
                    Node* ls = (Node*)st.get(leftId, false);
                    if (ls->keyCount > LEAF_MIN) {
                        Node l;
                        memcpy(&l, ls, sizeof(Node));
                        memmove(&node.leafKeys[1], &node.leafKeys[0], node.keyCount * sizeof(Key));
                        node.leafKeys[0] = l.leafKeys[l.keyCount - 1];
                        node.keyCount++;
                        l.keyCount--;
                        p.in.keys[ci - 1] = node.leafKeys[0];
                        memcpy(st.get(leftId, true), &l, sizeof(Node));
                        memcpy(st.get(nodeId, true), &node, sizeof(Node));
                        memcpy(st.get(pid, true), &p, sizeof(Node));
                        return;
                    }
                }
                if (rightId != -1) {
                    Node* rs = (Node*)st.get(rightId, false);
                    if (rs->keyCount > LEAF_MIN) {
                        Node r;
                        memcpy(&r, rs, sizeof(Node));
                        node.leafKeys[node.keyCount] = r.leafKeys[0];
                        node.keyCount++;
                        memmove(&r.leafKeys[0], &r.leafKeys[1], (r.keyCount - 1) * sizeof(Key));
                        r.keyCount--;
                        p.in.keys[ci] = r.leafKeys[0];
                        memcpy(st.get(rightId, true), &r, sizeof(Node));
                        memcpy(st.get(nodeId, true), &node, sizeof(Node));
                        memcpy(st.get(pid, true), &p, sizeof(Node));
                        return;
                    }
                }
                if (leftId != -1) {
                    // merge node into left
                    Node l;
                    memcpy(&l, st.get(leftId, false), sizeof(Node));
                    memcpy(&l.leafKeys[l.keyCount], node.leafKeys, node.keyCount * sizeof(Key));
                    l.keyCount += node.keyCount;
                    l.next = node.next;
                    memcpy(st.get(leftId, true), &l, sizeof(Node));
                    st.freeBlock(nodeId);
                    memmove(&p.in.keys[ci - 1], &p.in.keys[ci], (p.keyCount - ci) * sizeof(Key));
                    memmove(&p.in.children[ci], &p.in.children[ci + 1], (p.keyCount - ci) * sizeof(i64));
                    p.keyCount--;
                    memcpy(st.get(pid, true), &p, sizeof(Node));
                } else {
                    // merge right into node
                    Node r;
                    memcpy(&r, st.get(rightId, false), sizeof(Node));
                    memcpy(&node.leafKeys[node.keyCount], r.leafKeys, r.keyCount * sizeof(Key));
                    node.keyCount += r.keyCount;
                    node.next = r.next;
                    memcpy(st.get(nodeId, true), &node, sizeof(Node));
                    st.freeBlock(rightId);
                    memmove(&p.in.keys[ci], &p.in.keys[ci + 1], (p.keyCount - ci - 1) * sizeof(Key));
                    memmove(&p.in.children[ci + 1], &p.in.children[ci + 2], (p.keyCount - ci - 1) * sizeof(i64));
                    p.keyCount--;
                    memcpy(st.get(pid, true), &p, sizeof(Node));
                }
            } else {
                Node node;
                memcpy(&node, st.get(nodeId, false), sizeof(Node));
                if (leftId != -1) {
                    Node* ls = (Node*)st.get(leftId, false);
                    if (ls->keyCount > IN_MIN_KEYS) {
                        Node l;
                        memcpy(&l, ls, sizeof(Node));
                        memmove(&node.in.keys[1], &node.in.keys[0], node.keyCount * sizeof(Key));
                        memmove(&node.in.children[1], &node.in.children[0], (node.keyCount + 1) * sizeof(i64));
                        node.in.keys[0] = p.in.keys[ci - 1];
                        node.in.children[0] = l.in.children[l.keyCount];
                        p.in.keys[ci - 1] = l.in.keys[l.keyCount - 1];
                        l.keyCount--;
                        node.keyCount++;
                        memcpy(st.get(leftId, true), &l, sizeof(Node));
                        memcpy(st.get(nodeId, true), &node, sizeof(Node));
                        memcpy(st.get(pid, true), &p, sizeof(Node));
                        return;
                    }
                }
                if (rightId != -1) {
                    Node* rs = (Node*)st.get(rightId, false);
                    if (rs->keyCount > IN_MIN_KEYS) {
                        Node r;
                        memcpy(&r, rs, sizeof(Node));
                        node.in.keys[node.keyCount] = p.in.keys[ci];
                        node.in.children[node.keyCount + 1] = r.in.children[0];
                        p.in.keys[ci] = r.in.keys[0];
                        memmove(&r.in.keys[0], &r.in.keys[1], (r.keyCount - 1) * sizeof(Key));
                        memmove(&r.in.children[0], &r.in.children[1], r.keyCount * sizeof(i64));
                        r.keyCount--;
                        node.keyCount++;
                        memcpy(st.get(rightId, true), &r, sizeof(Node));
                        memcpy(st.get(nodeId, true), &node, sizeof(Node));
                        memcpy(st.get(pid, true), &p, sizeof(Node));
                        return;
                    }
                }
                if (leftId != -1) {
                    // merge node into left
                    Node l;
                    memcpy(&l, st.get(leftId, false), sizeof(Node));
                    l.in.keys[l.keyCount] = p.in.keys[ci - 1];
                    memcpy(&l.in.keys[l.keyCount + 1], node.in.keys, node.keyCount * sizeof(Key));
                    memcpy(&l.in.children[l.keyCount + 1], node.in.children, (node.keyCount + 1) * sizeof(i64));
                    l.keyCount += 1 + node.keyCount;
                    memcpy(st.get(leftId, true), &l, sizeof(Node));
                    st.freeBlock(nodeId);
                    memmove(&p.in.keys[ci - 1], &p.in.keys[ci], (p.keyCount - ci) * sizeof(Key));
                    memmove(&p.in.children[ci], &p.in.children[ci + 1], (p.keyCount - ci) * sizeof(i64));
                    p.keyCount--;
                    memcpy(st.get(pid, true), &p, sizeof(Node));
                } else {
                    // merge right into node
                    Node r;
                    memcpy(&r, st.get(rightId, false), sizeof(Node));
                    node.in.keys[node.keyCount] = p.in.keys[ci];
                    memcpy(&node.in.keys[node.keyCount + 1], r.in.keys, r.keyCount * sizeof(Key));
                    memcpy(&node.in.children[node.keyCount + 1], r.in.children, (r.keyCount + 1) * sizeof(i64));
                    node.keyCount += 1 + r.keyCount;
                    memcpy(st.get(nodeId, true), &node, sizeof(Node));
                    st.freeBlock(rightId);
                    memmove(&p.in.keys[ci], &p.in.keys[ci + 1], (p.keyCount - ci - 1) * sizeof(Key));
                    memmove(&p.in.children[ci + 1], &p.in.children[ci + 2], (p.keyCount - ci - 1) * sizeof(i64));
                    p.keyCount--;
                    memcpy(st.get(pid, true), &p, sizeof(Node));
                }
            }
            // parent lost one key after a merge; check for underflow
            if (d == 0) {
                if (p.keyCount == 0) {
                    i64 newRoot = p.in.children[0];
                    st.freeBlock(pid);
                    st.meta.root = newRoot;
                }
                return;
            }
            if (p.keyCount >= IN_MIN_KEYS) return;
            nodeId = pid;
            nodeIsLeaf = false;
            d--;
        }
    }

    void find(const char* index, string& out) const {
        if (st.meta.root == -1) { out += "null\n"; return; }
        Key start;
        memset(start.str, 0, 64);
        size_t L = strlen(index);
        if (L > 64) L = 64;
        memcpy(start.str, index, L);
        start.value = INT_MIN;
        BPT* self = const_cast<BPT*>(this);
        i64 cur = st.meta.root;
        while (true) {
            Node* n = (Node*)self->st.get(cur, false);
            if (n->isLeaf) break;
            cur = n->in.children[inChild(*n, start)];
        }
        bool foundAny = false;
        bool done = false;
        bool firstLeaf = true;
        while (cur != -1 && !done) {
            Node* n = (Node*)self->st.get(cur, false);
            int kc = n->keyCount;
            i64 nxt = n->next;
            int pos = 0;
            if (firstLeaf) { pos = leafLower(*n, start); firstLeaf = false; }
            for (; pos < kc; pos++) {
                if (memcmp(n->leafKeys[pos].str, start.str, 64) != 0) { done = true; break; }
                if (foundAny) out.push_back(' ');
                appendInt(out, n->leafKeys[pos].value);
                foundAny = true;
            }
            cur = nxt;
        }
        if (!foundAny) out += "null";
        out.push_back('\n');
    }

    static inline void appendInt(string& out, int v) {
        char tmp[12];
        int i = 0;
        unsigned int x;
        if (v < 0) { out.push_back('-'); x = (unsigned int)(-(i64)v); }
        else x = (unsigned int)v;
        do { tmp[i++] = (char)('0' + (x % 10)); x /= 10; } while (x);
        while (i) out.push_back(tmp[--i]);
    }
};

class Reader {
    static const int SZ = 1 << 20;
    char buf[SZ];
    int pos, len;
public:
    Reader() : pos(0), len(0) {}
    inline char getch() {
        if (pos == len) {
            len = (int)fread(buf, 1, SZ, stdin);
            pos = 0;
            if (len == 0) return 0;
        }
        return buf[pos++];
    }
    bool token(char* out) {
        char c;
        do {
            c = getch();
            if (!c) return false;
        } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
        int i = 0;
        while (c && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            out[i++] = c;
            c = getch();
        }
        out[i] = 0;
        return true;
    }
    bool readInt(int& v) {
        char c;
        do {
            c = getch();
            if (!c) return false;
        } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
        bool neg = false;
        if (c == '-') { neg = true; c = getch(); }
        long long x = 0;
        while (c >= '0' && c <= '9') { x = x * 10 + (c - '0'); c = getch(); }
        v = neg ? (int)(-x) : (int)x;
        return true;
    }
};

int main() {
    Reader rd;
    int n;
    if (!rd.readInt(n)) return 0;
    BPT tree("data.db");
    string out;
    out.reserve(1 << 20);
    char cmd[16];
    char idx[160];
    for (int i = 0; i < n; i++) {
        if (!rd.token(cmd)) break;
        char c0 = cmd[0];
        if (c0 == 'i') { // insert
            int v;
            rd.token(idx);
            rd.readInt(v);
            Key k;
            memset(k.str, 0, 64);
            size_t L = strlen(idx);
            if (L > 64) L = 64;
            memcpy(k.str, idx, L);
            k.value = v;
            tree.insert(k);
        } else if (c0 == 'd') { // delete
            int v;
            rd.token(idx);
            rd.readInt(v);
            Key k;
            memset(k.str, 0, 64);
            size_t L = strlen(idx);
            if (L > 64) L = 64;
            memcpy(k.str, idx, L);
            k.value = v;
            tree.erase(k);
        } else if (c0 == 'f') { // find
            rd.token(idx);
            tree.find(idx, out);
            if (out.size() > (1 << 22)) {
                fwrite(out.data(), 1, out.size(), stdout);
                out.clear();
            }
        }
    }
    if (!out.empty()) fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
    return 0;
}
