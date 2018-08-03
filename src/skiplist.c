#include "../include/skiplist.h"
#include "../include/btree.h"
#include <errno.h>

// _sl_names 通过前缀生成跳表所涉及的所有与文件相关的文件名及文件名前缀
static names_t* _sl_names(const char* prefix) {
    int n = 0;
    int prefix_len = strlen(prefix);

    names_t* ns = (names_t*)malloc(sizeof(names_t));

    n = prefix_len + 1;
    ns->prefix = (char*)malloc(sizeof(char) * n);
    snprintf(ns->prefix, n, "%s", prefix);

    n = prefix_len + sizeof(META_SUFFIX) + 1;
    ns->meta = (char*)malloc(sizeof(char) * n);
    snprintf(ns->meta , n, "%s%s", prefix, META_SUFFIX);

    n = prefix_len + sizeof(DATA_SUFFIX) + 1;
    ns->data = (char*)malloc(sizeof(char) * n);
    snprintf(ns->data, n, "%s%s", prefix, DATA_SUFFIX);

    n = prefix_len + sizeof(SPLIT_REDOLOG_SUFFIX) + 1;
    ns->redo = (char*)malloc(sizeof(char) * n);
    snprintf(ns->redo, n, "%s%s", prefix, SPLIT_REDOLOG_SUFFIX);

    n = prefix_len + sizeof(SPLIT_LEFT_SUFFIX) + 1;
    ns->left_prefix = (char*)malloc(sizeof(char) * n);
    snprintf(ns->left_prefix, n, "%s%s", prefix, SPLIT_LEFT_SUFFIX);

    n = prefix_len + sizeof(SPLIT_RIGHT_SUFFIX) + 1;
    ns->right_prefix = (char*)malloc(sizeof(char) * n);
    snprintf(ns->right_prefix, n, "%s%s", prefix, SPLIT_RIGHT_SUFFIX);

    return ns;
}

// _sl_names_free 释放ns申请的内存空间
static void _sl_names_free(names_t* ns) {
    if (ns == NULL) {
        return;
    }
    if (ns->prefix != NULL) {
        free(ns->prefix);
    }
    if (ns->meta != NULL) {
        free(ns->meta);
    }
    if (ns->data != NULL) {
        free(ns->data);
    }
    if (ns->redo != NULL) {
        free(ns->redo);
    }
    if (ns->left_prefix != NULL) {
        free(ns->left_prefix);
    }
    if (ns->right_prefix != NULL) {
        free(ns->right_prefix);
    }
    free(ns);
}

// find_and_reset_maxkey 查找并重置maxkey
static void find_and_reset_maxkey(skiplist_t* sl) {
    size_t size = 1;
    char* maxkey = (char*)malloc(sizeof(char));
    maxkey[0] = '\0';
    if (sl != NULL) {
        metanode_t* mnode = METANODE(sl, sl->meta->tail);
        if (mnode != NULL && ((mnode->flag & METANODE_HEAD) != METANODE_HEAD)) {
            free(maxkey);
            datanode_t* dnode = sl_get_datanode(sl, mnode->offset);
            maxkey = (char*)malloc(sizeof(char) * dnode->size);
            memcpy(maxkey, dnode->data, dnode->size);
            size = dnode->size;
        }
    }
    if (sl->split != NULL && sl->split->redolog != NULL) {
        char* redo_maxkey = NULL;
        size_t redo_size = 0;
        status_t st = ssl_get_maxkey(sl->split->redolog, (void**)&redo_maxkey, &redo_size);
        if (st.code == 0 && (compare(maxkey, size, redo_maxkey, redo_size) == -1)) {
            free(maxkey);
            maxkey = (char*)malloc(sizeof(char) * redo_size);
            memcpy(maxkey, redo_maxkey, redo_size);
            size = redo_size;
        }
        free(sl->maxkey);
        sl->maxkey = maxkey;
        sl->maxkey_len = size;
        return;
    }
    if (sl->split != NULL && sl->split->left != NULL) {
        metanode_t* mnode = METANODE(sl->split->left, sl->split->left->meta->tail);
        if (mnode != NULL && ((mnode->flag & METANODE_HEAD) != METANODE_HEAD)) {
            datanode_t* dnode = sl_get_datanode(sl->split->left, mnode->offset);
            if (compare(maxkey, size, dnode->data, dnode->size) == -1) {
                free(maxkey);
                maxkey = (char*)malloc(sizeof(char) * dnode->size);
                memcpy(maxkey, dnode->data, dnode->size);
                size = dnode->size;
            }
        }
    }
    if (sl->split != NULL && sl->split->right != NULL) {
        metanode_t* mnode = METANODE(sl->split->right, sl->split->right->meta->tail);
        if (mnode != NULL && ((mnode->flag & METANODE_HEAD) != METANODE_HEAD)) {
            datanode_t* dnode = sl_get_datanode(sl->split->right, mnode->offset);
            if (compare(maxkey, size, dnode->data, dnode->size) == -1) {
                free(maxkey);
                maxkey = (char*)malloc(sizeof(char) * dnode->size);
                memcpy(maxkey, dnode->data, dnode->size);
                size = dnode->size;
            }
        }
    }
    free(sl->maxkey);
    sl->maxkey = maxkey;
    sl->maxkey_len = size;
}

// sl_get_maxkey 获取跳表的maxkey 若跳表为空则返回跳表自身的maxkey（默认为'\0'）
status_t sl_get_maxkey(skiplist_t* sl, void** key, size_t* size) {
    status_t _status;
    uint64_t _offsets[] = {};

    _status = sl_rdlock(sl, _offsets, 0);
    if (_status.code != 0) {
        return _status;
    }
    *key = sl->maxkey;
    *size = sl->maxkey_len;
    sl_unlock(sl, _offsets, 0);
    return _status;
}

// sl_get_datanode 获取跳表数据文件中指定偏移所对应的datanode_t节点
inline datanode_t* sl_get_datanode(skiplist_t* sl, uint64_t offset) {
    return (datanode_t*)(sl->data->mapped + offset);
}

// createmeta 创建跳表元数据头
static void createmeta(skiplist_t* sl, void* mapped, uint64_t mapcap, float p) {
    sl->meta = (skipmeta_t*)mapped;
    sl->meta->mapcap = mapcap;
    sl->meta->mapped = mapped;
    sl->meta->mapsize = sizeof(skipmeta_t) + sizeof(metanode_t) + sizeof(uint64_t) * SKIPLIST_MAXLEVEL;
    sl->meta->tail = sizeof(skipmeta_t);
    sl->meta->count = 0;
    sl->meta->p = p;
    for (int i = 0; i < SKIPLIST_MAXLEVEL; ++i) {
        sl->metafree[i] = NULL;
    }
    metanode_t* head = (metanode_t*)(mapped + sizeof(skipmeta_t));
    head->flag = METANODE_HEAD;
    head->offset = 0;
    head->value = 0;
    head->backward = 0;
    head->level = 0;
}

// loadmeta 从映射的mapped中加载跳表元数据头并获取空闲节点列表
static void loadmeta(skiplist_t* sl, void* mapped, uint64_t mapcap) {
    sl->meta = (skipmeta_t*)mapped;
    sl->meta->mapcap = mapcap;
    sl->meta->mapped = mapped;

    // 获取元数据头中空闲节点并放入metafree中
    metanode_t* curr = (metanode_t*)(mapped + sizeof(skipmeta_t) + sizeof(metanode_t) + sizeof(uint64_t) * SKIPLIST_MAXLEVEL);
    while (curr != NULL && (curr->flag | METANODE_NONE)) {
        if ((curr->flag | METANODE_DELETED) == METANODE_DELETED) {
            if (sl->metafree[curr->level] == NULL) {
                list_create(&sl->metafree[curr->level]);
            }
            list_push_front(sl->metafree[curr->level], METANODEPOSITION(sl, curr)); // reload recycle meta space
        }
        // next
        curr = (metanode_t*)((void*)curr + sizeof(metanode_t) + sizeof(uint64_t) * curr->level);
        if ((void*)curr - mapped >= sl->meta->mapsize) {
            break;
        }
    }
}

// createdata 创建跳表数据文件头
static void createdata(skiplist_t* sl, void* mapped, uint64_t mapcap) {
    sl->data = (skipdata_t*)mapped;
    sl->data->mapped = mapped;
    sl->data->mapsize = sizeof(skipdata_t);
    sl->data->mapcap = mapcap;
    sl->datafree = NULL;
}

static int cmpu64(const void* p1, const void* p2) {
    return *((uint64_t*)p1) > *((uint64_t*)p2);
}

// loaddata 从映射的mapped中加载跳表数据头文件并获取已映射的空闲空间列表
static void loaddata(skiplist_t* sl, void* mapped, uint64_t mapcap) {
    sl->data = (skipdata_t*)mapped;
    sl->data->mapcap = mapcap;
    sl->data->mapped = mapped;

    uint64_t* offsets = (uint64_t*)malloc(sizeof(uint64_t) * sl->meta->count);
    for (int i = 0; i < sl->meta->count; ++i) {
        offsets[i] = 0;
    }
    // 获取meta结构中已使用的data中偏移量
    metanode_t* curr = METANODEHEAD(sl);
    for (int i = 0;; ++i) {
        metanode_t* next = METANODE(sl, curr->forwards[0]);
        if (next == NULL) {
            break;
        }
        offsets[i] = next->offset;
        curr = next;
    }
    qsort(offsets, sl->meta->count, sizeof(uint64_t*), cmpu64);
    datanode_t* dnode = (datanode_t*)(sl->data->mapped + sizeof(skipdata_t));
    // 通过已使用的空间，获取空闲的data空间
    for (int i = 0; i < sl->meta->count; ++i) {
        while (offsets[i] != DATANODEPOSITION(sl, dnode)) {
            if (sl->datafree == NULL) {
                list_create(&sl->datafree);
            }
            list_push_front(sl->datafree, DATANODEPOSITION(sl, dnode));
            dnode = (datanode_t*)((void*)dnode + sizeof(datanode_t) + dnode->size);
        }
        dnode = (datanode_t*)((void*)dnode + sizeof(datanode_t) + dnode->size);
    }
    free(offsets);
}

static status_t _skipsplit(skiplist_t* sl);

// loadsplit 加载分裂跳表
static status_t loadsplit(skiplist_t* sl) {
    int err;
    status_t _status = { .code = 0 };
    sl->split = (skipsplit_t*)malloc(sizeof(skipsplit_t));
    sl->split->redolog = NULL;
    sl->split->left = NULL;
    sl->split->right = NULL;

    // 若有redolog对应跳表仅加载redolog对应跳表
    if (access(sl->names->redo, F_OK) == 0) {
        _status = _skipsplit(sl);
        if (_status.code != 0) {
            if (sl->split->redolog != NULL) {
                ssl_close(sl->split->redolog);
                sl->split->redolog = NULL;
            }
            free(sl->split);
            return _status;
        }
        if ((err = pthread_join(sl->split_id, NULL)) != 0) {
            return statusfuncnotok(_status, err, "pthread_join");
        }
        find_and_reset_maxkey(sl);
        return _status;
    }
    // 若有left/right跳表则加载left/right跳表
    names_t* lns = _sl_names(sl->names->left_prefix);
    names_t* rns = _sl_names(sl->names->right_prefix);
    if (access(lns->meta, F_OK) == 0 && access(lns->data, F_OK == 0) && access(rns->meta, F_OK) == 0 && access(rns->data, F_OK == 0)) {
        _sl_names_free(lns);
        _sl_names_free(rns);
        _status = sl_open(sl->db, sl->names->left_prefix, sl->meta->p, &sl->split->left);
        if (_status.code != 0) {
            free(sl->split);
            return _status;
        }
        _status = sl_open(sl->db, sl->names->right_prefix, sl->meta->p, &sl->split->right);
        if (_status.code != 0) {
            sl_close(sl->split->left);
            free(sl->split);
            return _status;
        }
        find_and_reset_maxkey(sl);
        sl->state = SKIPLIST_STATE_SPLIT_DONE;
        return _status;
    }
    _sl_names_free(lns);
    _sl_names_free(rns);
    // 否则不存在分裂后的跳表
    free(sl->split);
    sl->split = NULL;
    return _status;
}

// _sl_load 加载跳表meta & data数据
status_t _sl_load(skiplist_t* sl) {
    status_t _status;
    uint64_t mapcap = 0;
    void* mapped = NULL;

    // load meta
    {
        _status = ommap(sl->names->meta, &mapcap, &mapped);
        if (_status.code != 0) {
            munmap(mapped, mapcap);
            return _status;
        }
        loadmeta(sl, mapped, mapcap);
    }
    // load data
    {
        _status = ommap(sl->names->data, &mapcap, &mapped);
        if (_status.code != 0) {
            munmap(mapped, mapcap);
            return _status;
        }
        loaddata(sl, mapped, mapcap);
    }
    find_and_reset_maxkey(sl);
    return _status;
}

// _sl_create 创建出跳表对应的meta&data
status_t _sl_create(skiplist_t* sl, float p) {
    status_t _status;
    void* mapped = NULL;

    // create meta
    {
        _status = cmmap(sl->names->meta, DEFAULT_METAFILE_SIZE, &mapped);
        if (_status.code != 0) {
            munmap(mapped, DEFAULT_METAFILE_SIZE);
            return _status;
        }
        createmeta(sl, mapped, DEFAULT_METAFILE_SIZE, p);
    }
    // create data
    {
        _status = cmmap(sl->names->data, DEFAULT_DATAFILE_SIZE, &mapped);
        if (_status.code != 0) {
            munmap(mapped, DEFAULT_DATAFILE_SIZE);
            return _status;
        }
        createdata(sl, mapped, DEFAULT_DATAFILE_SIZE);
    }
    return _status;
}

// _sl_new 新建跳表结构
status_t _sl_new(skipdb_t* db, const char* prefix, skiplist_t** sl) {
    int err;
    status_t _status = { .code = 0 };

    if (prefix == NULL) {
        return statusnotok0(_status, "prefix is NULL");
    }
    *sl = (skiplist_t*)malloc(sizeof(skiplist_t));
    (*sl)->meta = NULL;
    (*sl)->data = NULL;
    (*sl)->split = NULL;
    for (int i = 0; i < SKIPLIST_MAXLEVEL; ++i) {
        (*sl)->metafree[i] = NULL;
    }
    (*sl)->datafree = NULL;
    (*sl)->state = SKIPLIST_STATE_NORMAL;
    (*sl)->db = db;
    (*sl)->names = _sl_names(prefix);
    (*sl)->maxkey = (char*)malloc(sizeof(char));
    (*sl)->maxkey[0] = '\0';
    (*sl)->maxkey_len = 1;

    if ((err = pthread_rwlock_init(&(*sl)->rwlock, NULL)) != 0) {
        return statusfuncnotok(_status, err, "pthread_rwlock_init");
    }
    return _status;
}

// sl_create 创建跳表
static status_t sl_create(skipdb_t* db, const char* prefix, float p, skiplist_t** sl) {
    status_t _status;
    _status = _sl_new(db, prefix, sl);
    if (_status.code != 0) {
        sl_close(*sl);
        return _status;
    }
    _status = _sl_create(*sl, p);
    if (_status.code != 0) {
        sl_close(*sl);
        return _status;
    }
    return _status;
}

// sl_open 打开跳表（创建或加载已有跳表）
status_t sl_open(skipdb_t* db, const char* prefix, float p, skiplist_t** sl) {
    status_t _status;

    _status = _sl_new(db, prefix, sl);
    if (_status.code != 0) {
        sl_close(*sl);
        return _status;
    }
    int ret1 = access((*sl)->names->meta, F_OK);
    int ret2 = access((*sl)->names->data, F_OK);
    if (ret1 == 0 && ret2 == 0) {
        _status = _sl_load(*sl);
    } else if (ret1 != 0 && ret2 != 0) {
        _status = _sl_create(*sl, p);
    } else {
        sl_close(*sl);
        return statusnotok2(_status, "%s or %s not eixst", (*sl)->names->meta, (*sl)->names->data);
    }
    if (_status.code != 0) {
        sl_close(*sl);
        return _status;
    }
    _status = loadsplit(*sl);
    if (_status.code != 0) {
        sl_close(*sl);
        return _status;
    }
    return _status;
}

// run_skipsplit 将跳表分裂成 left/right 并将分裂过程中修改的新数据(redolog)同步至left/right
static void* run_skipsplit(void* arg) {
    uint64_t _offsets[] = {};
    skiplist_t* sl = (skiplist_t*)arg;
    // 按照当前跳表元素总数均等拆分（范围不一定相同）
    int lcount = sl->meta->count / 2;
    int rcount = sl->meta->count - lcount;

    // 左半部分插入sl->split->left
    metanode_t* curr = METANODEHEAD(sl);
    for (int i = 0; i < lcount; ++i) {
        metanode_t* next = METANODE(sl, curr->forwards[0]);
        if (next == NULL) {
            break;
        }
        datanode_t* dnode = sl_get_datanode(sl, next->offset);
        sl_put(sl->split->left, dnode->data, dnode->size, next->value);
        curr = next;
    }
    // 右半部分插入sl->split->lright
    for (int i = 0; i < rcount; ++i) {
        metanode_t* next = METANODE(sl, curr->forwards[0]);
        if (next == NULL) {
            break;
        }
        datanode_t* dnode = sl_get_datanode(sl, next->offset);
        sl_put(sl->split->right, dnode->data, dnode->size, next->value);
        curr = next;
    }

    // 锁定跳表，阻塞对sl->split->redolog操作
    sl_wrlock(sl, _offsets, 0);
    metanode_t* lmnode = METANODE(sl->split->left, sl->split->left->meta->tail);
    if (lmnode == NULL) {
        // NOTE: impossible
        metanode_t* rmnode = METANODE(sl->split->right, sl->split->right->meta->tail);
        if (rmnode == NULL) {
            sl_unlock(sl, _offsets, 0);
            return NULL;
        }
        sskipnode_t* ssnode = SSL_NODEHEAD(sl->split->redolog);
        while (1) {
            sskipnode_t* next = SSL_NODE(sl->split->redolog, ssnode->forwards[-1]);
            if (next == NULL) {
                break;
            }
            if (next->flag == SSL_NODE_USED) {
                sl_put(sl->split->right, next->key, next->key_len, next->value);
            } else if (next->flag == SSL_NODE_DELETED) {
                sl_del(sl->split->right, next->key, next->key_len);
            }
            ssnode = next;
        }
        sl_unlock(sl, _offsets, 0);
        return NULL;
    }
    // 获取sl->split->left最大key
    datanode_t* ldnode = sl_get_datanode(sl->split->left, lmnode->offset);
    char* key = (char*)malloc(sizeof(char) * ldnode->size);
    memcpy(key, ldnode->data, ldnode->size);
    size_t size = ldnode->size;
    sskipnode_t* ssnode = SSL_NODEHEAD(sl->split->redolog);
    // 移动sl->split->redolog中key至sl->split->left, sl->split->right
    while (1) {
        sskipnode_t* next = SSL_NODE(sl->split->redolog, ssnode->forwards[-1]);
        if (next == NULL) {
            break;
        }
        skiplist_t* seleted = NULL;
        switch (compare(next->key, next->key_len, key, size)) {
            case 1: // > left max key 移动至 sl->split->right
                seleted = sl->split->right;
                break;
            default: // <= left max key 移动至 sl->split->left
                seleted = sl->split->left;
        }
        if (next->flag == SSL_NODE_USED) {
            sl_put(seleted, next->key, next->key_len, next->value);
        } else if (next->flag == SSL_NODE_DELETED) {
            // 将sl->split->redolog中惰性删除节点反应至left/right跳表中
            sl_del(seleted, next->key, next->key_len);
        }
        ssnode = next;
    }
    free(key);
    // 重置left/right中max key
    find_and_reset_maxkey(sl->split->left);
    find_and_reset_maxkey(sl->split->right);
    // 将状态标记为：跳表分裂完成
    sl->state = SKIPLIST_STATE_SPLIT_DONE;
    // 释放并移除sl->split->redolog
    ssl_destroy(sl->split->redolog);
    sl->split->redolog = NULL;
    sl_unlock(sl, _offsets, 0);
    pthread_exit((void *)0);
    return NULL;
}

// _skipsplit 创建出redolog/left/right跳表，新建跳表分裂线程去执行分裂，并更改跳表状态为SKIPLIST_STATE_SPLITED
static status_t _skipsplit(skiplist_t* sl) {
    int err;
    status_t _status;

    // 新建redolog，用于记录分裂过程中的修改操作
    _status = ssl_open(sl->names->redo, sl->meta->p, &sl->split->redolog);
    if (_status.code != 0) {
        return _status;
    }
    // 创建分裂后跳表结构left
    _status = sl_create(sl->db, sl->names->left_prefix, sl->meta->p, &sl->split->left);
    if (_status.code != 0) {
        return _status;
    }
    sl->split->left->state = SKIPLIST_STATE_SPLITER;
    // 创建分裂后跳表结构right
    _status = sl_create(sl->db, sl->names->right_prefix, sl->meta->p, &sl->split->right);
    if (_status.code != 0) {
        sl_destroy(sl->split->left);
        return _status;
    }
    sl->split->right->state = SKIPLIST_STATE_SPLITER;
    // 启动分裂线程
    if ((err = pthread_create(&sl->split_id, NULL, run_skipsplit, sl)) != 0) {
        sl_destroy(sl->split->left);
        sl_destroy(sl->split->right);
        return statusfuncnotok(_status, err, "pthread_create");
    }
    sl->state = SKIPLIST_STATE_SPLITED;
    return _status;
}

// sl_sync 同步刷新跳表至磁盘
status_t sl_sync(skiplist_t* sl) {
    status_t _status = { .code = 0 };
    if (sl == NULL) {
        return _status;
    }
    if (sl->meta != NULL && sl->meta->mapped != NULL) {
        if (msync(sl->meta->mapped, sl->meta->mapcap, MS_SYNC) != 0) {
            return statusfuncnotok(_status, errno, "msync");
        }
    }
    if (sl->meta != NULL && sl->data->mapped != NULL) {
        if (msync(sl->data->mapped, sl->data->mapcap, MS_SYNC) != 0) {
            return statusfuncnotok(_status, errno, "msync");
        }
    }
    if (sl->split != NULL) {
        if (sl->split->redolog != NULL) {
            _status = ssl_sync(sl->split->redolog);
            if (_status.code != 0) {
                return _status;
            }
        }
        if (sl->split->left != NULL) {
            _status = sl_sync(sl->split->left);
            if (_status.code != 0) {
                return _status;
            }
        }
        if (sl->split->left != NULL) {
            _status = sl_sync(sl->split->left);
            if (_status.code != 0) {
                return _status;
            }
        }
    }
    return _status;
}

// TODO: 可能会影响碎片整理
// expandmetafile 扩展meta文件并重新映射meta
static status_t expandmetafile(skiplist_t* sl) {
    uint64_t newcap = 0;
    void* newmapped = NULL;
    status_t _status = { .code = 0 };

    if (sl->meta->mapcap < 1073741824) { // 1G: 1024 * 1024 * 1024
        newcap = sl->meta->mapcap * 2;
    } else {
        newcap = sl->meta->mapcap + 1073741824;
    }
    _status = ofmremap(sl->names->meta, sl->meta->mapped, sl->meta->mapcap, newcap, &newmapped);
    if (_status.code != 0) {
        return _status;
    }
    loadmeta(sl, newmapped, newcap);
    return _status;
}

// TODO: 可能会影响碎片整理
// expanddatafile 扩展data文件并重新映射data
static status_t expanddatafile(skiplist_t* sl) {
    uint64_t newcap = 0;
    void* newmapped = NULL;
    status_t _status = { .code = 0 };

    if (sl->data->mapcap < 1073741824) { // 1G: 1024 * 1024 * 1024
        newcap = sl->data->mapcap * 2;
    } else {
        newcap = sl->data->mapcap + 1073741824;
    }
    _status = ofmremap(sl->names->data, sl->data->mapped, sl->data->mapcap, newcap, &newmapped);
    if (_status.code != 0) {
        return _status;
    }
    sl->data = (skipdata_t*)newmapped;
    sl->data->mapped = newmapped;
    sl->data->mapcap = newcap;
    return _status;
}

// skipsplit_and_put 启动分裂线程并将数据put至redolog中
static status_t skipsplit_and_put(skiplist_t* sl, const void* key, size_t key_len, uint64_t value) {
    status_t _status = { .code = 0 };

    sl->split = (skipsplit_t*)malloc(sizeof(skipsplit_t));
    if (sl->split == NULL) {
        return statusfuncnotok(_status, errno, "malloc");
    }
    _status = _skipsplit(sl);
    if (_status.code != 0) {
        if (sl->split->redolog != NULL) {
            ssl_destroy(sl->split->redolog);
            sl->split->redolog = NULL;
        }
        return _status;
    }
    _status = ssl_put(sl->split->redolog, key, key_len, value);
    if (_status.code != 0) {
        return _status;
    }
    return _status;
}

// sl_rename 重命名跳表映射的文件名称
static void sl_rename(skiplist_t *sl, const char* prefix) {
    names_t* ns = _sl_names(prefix);
    rename(sl->names->meta, ns->meta);
    rename(sl->names->data, ns->data);
    _sl_names_free(sl->names);
    sl->names = ns;
}

// notify_btree_split 通知B树分裂并重命名left/right
static status_t notify_btree_split(skiplist_t* sl) {
    char* prefix;
    uint64_t _offsets[] = {};
    status_t _status = { .code = 0 };
    btree_str_t ostr, lstr, rstr;

    if (sl->db == NULL) {
        return statusnotok0(_status, "skiplist->db is NULL");
    }
    ostr.data = sl->maxkey;
    ostr.size = sl->maxkey_len;
    _status = sl_get_maxkey(sl->split->left, (void**)&lstr.data, &lstr.size);
    if (_status.code != 0) {
        return _status;
    }
    _status = sl_get_maxkey(sl->split->right, (void**)&rstr.data, &rstr.size);
    if (_status.code != 0) {
        return _status;
    }

    btree_split_cb(sl->db->btree, ostr, lstr, sl->split->left, rstr, sl->split->right);

    sl_wrlock(sl->split->left, _offsets, 0);
    sl->split->left->state = SKIPLIST_STATE_NORMAL;
    prefix = skipdb_get_next_filename(sl->db);
    sl_rename(sl->split->left, prefix);
    free(prefix);
    sl_unlock(sl->split->left, _offsets, 0);
    sl->split->left = NULL;

    sl_wrlock(sl->split->right, _offsets, 0);
    sl->split->right->state = SKIPLIST_STATE_NORMAL;
    prefix = skipdb_get_next_filename(sl->db);
    sl_rename(sl->split->right, prefix);
    free(prefix);
    sl_unlock(sl->split->right, _offsets, 0);
    sl->split->right = NULL;

    return _status;
}

// sl_put put新key至跳表
status_t sl_put(skiplist_t* sl, const void* key, size_t key_len, uint64_t value) {
    status_t _status = { .code = 0 };
    metanode_t* head = NULL;
    metanode_t* curr = NULL;
    metanode_t* update[SKIPLIST_MAXLEVEL] = { NULL };
    uint64_t _offsets[] = {};

    if (sl == NULL || key == NULL) {
        return statusnotok0(_status, "skiplist or key is NULL");
    }
    if (key_len > MAX_KEY_LEN) {
        return statusnotok2(_status, "key_len(%ld) over MAX_KEY_LEN(%d)", key_len, MAX_KEY_LEN);
    }
    _status = sl_wrlock(sl, _offsets, 0);
    if (_status.code != 0) {
        return _status;
    }
    // 若跳表是为被分裂者，则所有写操作写向redolog
    if (sl->state == SKIPLIST_STATE_SPLITED) {
        _status = ssl_put(sl->split->redolog, key, key_len, value);
        sl_unlock(sl, _offsets, 0);
        return _status;
    }
    // 若跳表已分裂完成，则put至left/right并通知btree分裂，分裂后销毁当前跳表
    if (sl->state == SKIPLIST_STATE_SPLIT_DONE) {
        metanode_t* mnode = METANODE(sl->split->left, sl->split->left->meta->tail);
        if (mnode == NULL) {
            _status = sl_put(sl->split->right, key, key_len, value);
            sl_unlock(sl, _offsets, 0);
            return _status;
        }
        datanode_t* dnode = sl_get_datanode(sl->split->left, mnode->offset);
        switch (compare(key, key_len, dnode->data, dnode->size)) {
            case 1:
                _status = sl_put(sl->split->right, key, key_len, value);
                break;
            default:
                _status = sl_put(sl->split->left, key, key_len, value);
        }
        if (_status.code != 0) {
            sl_unlock(sl, _offsets, 0);
            return _status;
        }
        // 通知上层分裂完成，上层操作完成后需通知下层重置标记位
        _status = notify_btree_split(sl);
        if (_status.code != 0) {
            sl_unlock(sl, _offsets, 0);
            return _status;
        }
        sl_unlock(sl, _offsets, 0);
        sl_destroy(sl);
        return _status;
    }
    uint16_t level = random_level(SKIPLIST_MAXLEVEL, sl->meta->p);
    // 判断是否达跳表容量上限
    if (sl->meta->mapcap - sl->meta->mapsize < (sizeof(metanode_t) + sizeof(uint64_t) * level)) {
        // 跳表为SKIPLIST_STATE_NORMAL时才可进行分裂操作
        if (sl->state == SKIPLIST_STATE_NORMAL) {
            _status = skipsplit_and_put(sl, key, key_len, value);
            sl_unlock(sl, _offsets, 0);
            return _status;
        }
        // 若操作的跳表为分裂者，则扩容，分裂者状态不会触发分裂
        if (sl->state == SKIPLIST_STATE_SPLITER) {
            _status = expandmetafile(sl);
            if (_status.code != 0) {
                sl_unlock(sl, _offsets, 0);
                return _status;
            }
        } else {
            sl_unlock(sl, _offsets, 0);
            return statusnotok1(_status, "current state(%d) unable expand meta file", sl->state);
        }
    }

    // 查找待插入节点及待更新节点列表
    head = curr = METANODEHEAD(sl);
    for (int level = curr->level - 1; level >= 0; --level) {
        while (1) {
            metanode_t* next = METANODE(sl, curr->forwards[level]);
            if (next == NULL) {
                break;
            }
            datanode_t* dnode = sl_get_datanode(sl, next->offset);
            int cmp = compare(dnode->data, dnode->size, key, key_len);
            if (cmp == 0) {
                next->value = value;
                return sl_unlock(sl, _offsets, 0);
            }
            if (cmp == -1) {
                curr = next;
                continue;
            }
            break;
        }
        update[level] = curr;
    }

    // 若metafree有空闲则重利用空间
    metanode_t* mnode = NULL;
    if (sl->metafree[level] != NULL && sl->metafree[level]->head != NULL) {
        listnode_t* reuse = NULL;
        list_front(sl->metafree[level], &reuse);
        mnode = (metanode_t*)(sl->meta->mapped + reuse->value);
        list_remove(sl->metafree[level], reuse);
    } else {
        mnode = (metanode_t*)(sl->meta->mapped + sl->meta->mapsize);
    }
    mnode->level = level;
    mnode->flag = METANODE_USED;
    mnode->offset = sl->data->mapsize;
    mnode->value = value;
    mnode->backward = METANODEPOSITION(sl, curr);
    for (int i = 0; i < mnode->level; ++i) {
        mnode->forwards[i] = 0;
    }

    // 若data已映射文件大小不足以存放新key，则扩大文件并重新映射
    if (sl->data->mapcap - sl->data->mapsize < sizeof(datanode_t) + MAX_KEY_LEN) {
        _status = expanddatafile(sl);
        if (_status.code != 0) {
            sl_unlock(sl, _offsets, 0);
            return _status;
        }
    }
    datanode_t* dnode = sl_get_datanode(sl, sl->data->mapsize);
    dnode->offset = METANODEPOSITION(sl, mnode);
    dnode->size = key_len;
    memcpy((void*)dnode->data, key, key_len);
    sl->data->mapsize += DATANODESIZE(dnode);

    if (head->level < mnode->level) {
        for (int i = head->level; i < mnode->level; ++i) {
            update[i] = head;
        }
        head->level = mnode->level;
    }
    // 更新prev的forwards 及 next的backend
    if (update[0] != NULL) {
        metanode_t* next = METANODE(sl, update[0]->forwards[0]);
        if (next != NULL) {
            next->backward = METANODEPOSITION(sl, mnode);
        } else {
            sl->meta->tail = METANODEPOSITION(sl, mnode);
        }
    }
    for (int i = 0; i < mnode->level; ++i) {
        mnode->forwards[i] = update[i]->forwards[i];
        update[i]->forwards[i] = METANODEPOSITION(sl, mnode);
    }
    sl->meta->count++;
    sl->meta->mapsize += METANODESIZE(mnode);
    return sl_unlock(sl, _offsets, 0);
}

// sl_get 获取跳表中指定key对应的数据
status_t sl_get(skiplist_t* sl, const void* key, size_t key_len, uint64_t* value) {
    status_t _status = { .code = 0 };
    uint64_t _offsets[] = {};

    if (sl == NULL || key == NULL) {
        return statusnotok0(_status, "skiplist or key is NULL");
    }
    _status = sl_rdlock(sl, _offsets, 0);
    if (_status.code != 0) {
        return _status;
    }
    // 若被操作的跳表已分裂完成，但上层并未开始分裂，则从left/right中查询
    if (sl->state == SKIPLIST_STATE_SPLIT_DONE) {
        metanode_t* mnode = METANODE(sl->split->left, sl->split->left->meta->tail);
        if (mnode == NULL) {
            _status = sl_get(sl->split->right, key, key_len, value);
            sl_unlock(sl, _offsets, 0);
            return _status;
        }
        datanode_t* dnode = sl_get_datanode(sl->split->left, mnode->offset);
        switch (compare(key, key_len, dnode->data, dnode->size)) {
            case -1:
                _status = sl_get(sl->split->left, key, key_len, value);
                break;
            case 1:
                _status = sl_get(sl->split->right, key, key_len, value);
                break;
            default:
                *value = mnode->value;
        }
        sl_unlock(sl, _offsets, 0);
        return _status;
    }
    // 若被操作的跳表正在分裂中，则优先搜索redolog
    if (sl->state == SKIPLIST_STATE_SPLITED) {
        sskipnode_t* snode = NULL;
        _status = ssl_getnode(sl->split->redolog, key, key_len, &snode);
        if (_status.code != 0) {
            sl_unlock(sl, _offsets, 0);
            return _status;
        }
        if (snode != NULL) {
            // 若在redolog中数据已被删除则不再向下查询
            if ((snode->flag & SSL_NODE_DELETED) == SSL_NODE_DELETED) {
                _status.code = STATUS_SKIPLIST_KEY_NOTFOUND;
                sl_unlock(sl, _offsets, 0);
                return _status;
            }
            *value = snode->value;
            return sl_unlock(sl, _offsets, 0);
        }
    }
    // 在原始跳表中查询
    metanode_t* curr = METANODEHEAD(sl);
    for (int level = curr->level - 1; level >= 0; --level) {
        while (1) {
            metanode_t* next = METANODE(sl, curr->forwards[level]);
            if (next == NULL) {
                break;
            }
            datanode_t* dnode = sl_get_datanode(sl, next->offset);
            int cmp = compare(dnode->data, dnode->size, key, key_len);
            if (cmp == -1) {
                curr = next;
                continue;
            }
            if (cmp == 0) {
                *value = next->value;
                return sl_unlock(sl, _offsets, 0);
            }
            break;
        }
    }
    _status.code = STATUS_SKIPLIST_KEY_NOTFOUND;
    sl_unlock(sl, _offsets, 0);
    return _status;
}

// sl_del 删除跳表中指定key对应的数据
status_t sl_del(skiplist_t* sl, const void* key, size_t key_len) {
    status_t _status = { .code = 0 };
    uint64_t _offsets[] = {};
    metanode_t* mnode = NULL;
    metanode_t* update[SKIPLIST_MAXLEVEL] = { NULL };

    if (sl == NULL || key == NULL) {
        return statusnotok0(_status, "skiplist or key is NULL");
    }
    _status = sl_wrlock(sl, _offsets, 0);
    if (_status.code != 0) {
        return _status;
    }
    // 若跳表处于分裂状态中，则将删除操作加入到redolog中
    if (sl->state == SKIPLIST_STATE_SPLITED) {
        _status = ssl_delput(sl->split->redolog, key, key_len);
        sl_unlock(sl, _offsets, 0);
        return _status;
    }
    // 若跳表以分裂完成，但上层还未分裂，则将删除操作推至left/right中
    if (sl->state == SKIPLIST_STATE_SPLIT_DONE) {
        metanode_t* mnode = METANODE(sl->split->left, sl->split->left->meta->tail);
        if (mnode == NULL) {
            _status = sl_del(sl->split->right, key, key_len);
            sl_unlock(sl, _offsets, 0);
            return _status;
        }
        datanode_t* dnode = sl_get_datanode(sl->split->left, mnode->offset);
        switch (compare(key, key_len, dnode->data, dnode->size)) {
            case 1:
                _status = sl_del(sl->split->right, key, key_len);
                break;
            default:
                _status = sl_del(sl->split->left, key, key_len);
        }
        sl_unlock(sl, _offsets, 0);
        return _status;
    }
    // 找出跳表中待删除节点及前后待更新节点
    metanode_t* curr = METANODEHEAD(sl);
    for (int level = curr->level - 1; level >= 0; --level) {
        while (1) {
            metanode_t* next = METANODE(sl, curr->forwards[level]);
            if (next == NULL) {
                break;
            }
            datanode_t* dnode = sl_get_datanode(sl, next->offset);
            int cmp = compare(dnode->data, dnode->size, key, key_len);
            if (cmp == -1) {
                curr = next;
                continue;
            }
            if (cmp == 1) {
                break;
            }
            update[level] = curr;
            mnode = next;
            break; // go to next level to find update[level-1]
        }
    }
    if (mnode == NULL) {
        return sl_unlock(sl, _offsets, 0);
    }
    // 更新prev的forwards 及 next的backend 及 跳表的tail
    for (int i = 0; i < mnode->level; ++i) {
        update[i]->forwards[i] = mnode->forwards[i];
    }
    if (mnode->forwards[0] != 0) {
        metanode_t* next = METANODE(sl, mnode->forwards[0]);
        next->backward = mnode->backward;
    } else {
        sl->meta->tail = mnode->backward;
    }
    curr = METANODEHEAD(sl);
    if (curr->forwards[mnode->level - 1] == METANODEPOSITION(sl, mnode) && mnode->forwards[mnode->level - 1] == 0) {
        --curr->level;
    }
    mnode->flag = METANODE_DELETED;
    --sl->meta->count;
    // 空闲data加入datafree
    list_push_front(sl->datafree, mnode->offset); // unused
    // 空间meta回收重利用
    list_push_front(sl->metafree[mnode->level], METANODEPOSITION(sl, mnode)); // recycle meta space
    return sl_unlock(sl, _offsets, 0);
}

status_t sl_rdlock(skiplist_t* sl, uint64_t offsets[], size_t offsets_n) {
    int err;
    status_t _status = { .code = 0 };

    if (sl == NULL) {
        return statusnotok0(_status, "skiplist is NULL");
    }
    if ((err = pthread_rwlock_rdlock(&sl->rwlock)) != 0) {
        return statusfuncnotok(_status, err, "pthread_rwlock_rdlock");
    }
    return _status;
}

status_t sl_wrlock(skiplist_t* sl, uint64_t offsets[], size_t offsets_n) {
    int err;
    status_t _status = { .code = 0 };

    if (sl == NULL) {
        return statusnotok0(_status, "skiplist is NULL");
    }
    if ((err = pthread_rwlock_wrlock(&sl->rwlock)) != 0) {
        return statusfuncnotok(_status, err, "pthread_rwlock_wrlock");
    }
    return _status;
}

status_t sl_unlock(skiplist_t* sl, uint64_t offsets[], size_t offsets_n) {
    int err;
    status_t _status = { .code = 0 };

    if (sl == NULL) {
        return statusnotok0(_status, "skiplist is NULL");
    }
    if ((err = pthread_rwlock_unlock(&sl->rwlock)) != 0) {
        return statusfuncnotok(_status, err, "pthread_rwlock_unlock");
    }
    return _status;
}

// _sl_close 关闭跳表并释放相应内存，若指定is_remove_file则按需删除跳表所有数据文件
status_t _sl_close(skiplist_t* sl, int is_remove_file) {
    int err;
    status_t _status = { .code = 0 };

    if (sl == NULL) {
        return _status;
    }
    // 关闭跳表时，等待跳表完成自身的分裂操作
    if (sl->split != NULL && sl->state != SKIPLIST_STATE_SPLIT_DONE) {
        if ((err = pthread_join(sl->split_id, NULL)) != 0) {
            return statusfuncnotok(_status, err, "pthread_join");
        }
    }
    // 若跳表已空，则删除跳表关联文件
    if (sl->meta != NULL && sl->meta->count == 0) {
        is_remove_file = 1;
    }
    // 关闭时主动刷盘
    sl_sync(sl);
    if (sl->meta != NULL && sl->meta->mapped != NULL) {
        if (munmap(sl->meta->mapped, sl->meta->mapsize) == -1) {
            return statusfuncnotok(_status, errno, "munmap");
        }
    }
    if (sl->meta != NULL && sl->data->mapped != NULL) {
        if (munmap(sl->data->mapped, sl->data->mapsize) == -1) {
            return statusfuncnotok(_status, errno, "munmap");
        }
    }
    if (sl->names != NULL) {
        if (is_remove_file) {
            remove(sl->names->meta);
        }
        if (is_remove_file) {
            remove(sl->names->data);
        }
        _sl_names_free(sl->names);
    }
    if (sl->maxkey != NULL) {
        free(sl->maxkey);
    }
    for (int i = 0; i < SKIPLIST_MAXLEVEL; i++) {
        if (sl->metafree[i] != NULL) {
            list_free(sl->metafree[i]);
        }
    }
    if (sl->datafree != NULL) {
        list_free(sl->datafree);
    }
    if ((err = pthread_rwlock_destroy(&sl->rwlock)) != 0) {
        return statusfuncnotok(_status, err, "pthread_rwlock_destroy");
    }
    if (sl->split != NULL) {
        // 关闭时保留left/right数据文件，移除redolog
        if (sl->split->left != NULL) {
            _status = _sl_close(sl->split->left, is_remove_file);
            if (_status.code != 0) {
                return _status;
            }
        }
        if (sl->split->right != NULL) {
            _status = _sl_close(sl->split->right, is_remove_file);
            if (_status.code != 0) {
                return _status;
            }
        }
        if (sl->split->redolog != NULL) {
            _status = ssl_destroy(sl->split->redolog);
            if (_status.code != 0) {
                return _status;
            }
        }
    }
    free(sl);
    return _status;
}

// sl_close 关闭跳表（保留数据文件）
status_t sl_close(skiplist_t* sl) {
    return _sl_close(sl, 0);
}

// sl_destroy 销毁跳表（删除数据文件）
status_t sl_destroy(skiplist_t* sl) {
    return _sl_close(sl, 1);
}
