#pragma once

#include <string>

namespace poq {

/// One property edit row. `address` is the write target (the slot address in
/// game memory); the other four fields are the values written there. Mirrors
/// the C# `ItemProperty` class (without the INotifyPropertyChanged plumbing,
/// which Qt's QTableWidget replaces).
class ItemProperty {
public:
    std::string address;  // hex string, e.g. "00AB1234"
    int index = 0;
    int num = 0;
    int color = 0;
    int precent = 0;

    ItemProperty() = default;
    ItemProperty(std::string addr, int idx, int n, int c, int p)
        : address(std::move(addr)), index(idx), num(n), color(c), precent(p) {}
};

}  // namespace poq
