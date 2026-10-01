// =============================================================================
//  ty.cpp : 类型表 —— 结构等价的类型全局唯一（intern 化）
//
//  类型是编译器各阶段共享的不可变对象：比较两个类型相等只需要比较指针。
//  复合类型（Fn / Array / Tuple / Named）目前尚未开放到语法层，但类型表
//  已就绪，后续加入数组、struct、错误通道时只需在 TyKind 上新增并挂载。
// =============================================================================
#include "lux.hpp"
#include <unordered_map>

namespace lux {

namespace {

struct TyPool {
    // 0.5.1：类型数量随数组 / 函数签名开始增长，intern 从 O(n) 线性扫描
    // 换成 hash 查找 O(1)。key 与 same() 的比较字段保持一致。
    struct TyKey {
        TyKind kind;
        const Ty* elem;
        std::string name;
        std::vector<const Ty*> members;
        bool operator==(const TyKey& o) const {
            return kind == o.kind && elem == o.elem && name == o.name &&
                   members == o.members;
        }
    };
    struct TyKeyHash {
        size_t operator()(const TyKey& k) const {
            size_t h = std::hash<int>{}(static_cast<int>(k.kind));
            h ^= std::hash<const void*>{}(k.elem) + 0x9e3779b9 + (h << 6) +
                 (h >> 2);
            h ^= std::hash<std::string>{}(k.name) + 0x9e3779b9 + (h << 6) +
                 (h >> 2);
            for (const Ty* m : k.members) {
                h ^= std::hash<const void*>{}(m) + 0x9e3779b9 + (h << 6) +
                     (h >> 2);
            }
            return h;
        }
    };

    std::vector<std::unique_ptr<Ty>> items;  // 保持稳定地址（Ty* 指入此处）
    std::unordered_map<TyKey, const Ty*, TyKeyHash> lookup;

    static TyKey keyOf(const Ty& t) {
        return TyKey{t.kind, t.elem, t.name, t.members};
    }

    const Ty* intern(Ty&& t) {
        TyKey k = keyOf(t);
        auto it = lookup.find(k);
        if (it != lookup.end()) return it->second;
        items.push_back(std::make_unique<Ty>(std::move(t)));
        const Ty* p = items.back().get();
        lookup.emplace(std::move(k), p);
        return p;
    }
};

TyPool& pool() {
    static TyPool p;
    return p;
}

}  // namespace

const Ty* TyStore::invalid() { return pool().intern(Ty{TyKind::Invalid, nullptr, {}, ""}); }
const Ty* TyStore::int64Ty() { return pool().intern(Ty{TyKind::Int, nullptr, {}, ""}); }
const Ty* TyStore::float64Ty() { return pool().intern(Ty{TyKind::Float, nullptr, {}, ""}); }
const Ty* TyStore::boolTy() { return pool().intern(Ty{TyKind::Bool, nullptr, {}, ""}); }
const Ty* TyStore::stringTy() { return pool().intern(Ty{TyKind::String, nullptr, {}, ""}); }
const Ty* TyStore::voidTy() { return pool().intern(Ty{TyKind::Void, nullptr, {}, ""}); }

const Ty* TyStore::fnOf(const Ty* ret, std::vector<const Ty*> params) {
    return pool().intern(Ty{TyKind::Fn, ret, std::move(params), ""});
}

const Ty* TyStore::arrayOf(const Ty* elem) {
    return pool().intern(Ty{TyKind::Array, elem, {}, ""});
}

const Ty* TyStore::tupleOf(std::vector<const Ty*> members) {
    return pool().intern(Ty{TyKind::Tuple, nullptr, std::move(members), ""});
}

const Ty* TyStore::named(const std::string& name) {
    return pool().intern(Ty{TyKind::Named, nullptr, {}, name});
}

const Ty* TyStore::optionalOf(const Ty* elem) {
    return pool().intern(Ty{TyKind::Optional, elem, {}, ""});
}

std::string tyName(const Ty* t) {
    if (!t) return "<未知类型>";
    switch (t->kind) {
        case TyKind::Invalid: return "<未知类型>";
        case TyKind::Int:     return "int";
        case TyKind::Float:   return "float";
        case TyKind::Bool:    return "bool";
        case TyKind::String:  return "string";
        case TyKind::Void:    return "void";
        case TyKind::Named:   return t->name;
        case TyKind::Array:   return tyName(t->elem) + "[]";
        case TyKind::Optional: return tyName(t->elem) + "?";
        case TyKind::Fn: {
            std::string s = "fn(";
            for (size_t i = 0; i < t->members.size(); i++) {
                if (i) s += ", ";
                s += tyName(t->members[i]);
            }
            return s + ") -> " + tyName(t->elem);
        }
        case TyKind::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < t->members.size(); i++) {
                if (i) s += ", ";
                s += tyName(t->members[i]);
            }
            return s + ")";
        }
    }
    return "<未知类型>";
}

bool isNumeric(const Ty* t) {
    return t && (t->kind == TyKind::Int || t->kind == TyKind::Float);
}

}  // namespace lux
