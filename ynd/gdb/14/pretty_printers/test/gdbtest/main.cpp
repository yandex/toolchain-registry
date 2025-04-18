#include <array>
#include <bitset>
#include <deque>
#include <exception>
#include <forward_list>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <util/generic/hash.h>
#include <util/generic/hash_multi_map.h>
#include <util/generic/hash_set.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/generic/maybe.h>
#include <util/system/yassert.h>
#include <library/cpp/enumbitset/enumbitset.h>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value.hpp>

std::tuple<int, int> test_tuple = {1, 2};
std::tuple<> test_empty_tuple;

std::vector<int> test_vector = {1, 2, 3, 4};

std::bitset<8> test_bitset(0xA5);

std::map<int, int> test_map = {{1, 2}, {3, 4}, {5, 6}, {7, 8}, {9, 10}};

std::multimap<int, int> test_multimap = {{1, 2}, {3, 4}, {1, 4}};

std::set<int> test_set = {1, 2, 3, 4, 5, 6};

std::unordered_map<int, int> test_unordered_map = {{1, 2}};
std::unordered_set<int> test_unordered_set = {1};

std::deque<int> test_deque = {1, 2, 3, 4};

std::pair<int, int> test_pair = {1, 2};

std::array<int, 4> test_array = {{1, 2, 3, 4}};

std::string test_string = "Это тест.";
std::string test_long_string = "A very long string that will not use small string optimization...";
std::string test_nullbyte_string{"\x00тест"sv};
std::string test_binary_string{"\xff\x00\xff\x00"sv};
std::string_view test_string_view(test_string);
std::string_view test_binary_string_view(test_binary_string);

std::unique_ptr<int> test_unique_int(new int(1));
std::unique_ptr<int> test_unique_empty;

std::shared_ptr<int> test_shared_int(new int(1));
std::shared_ptr<int> test_shared_empty;

std::variant<int, std::string> test_variant_default;  // Untested.
std::variant<int, std::string> test_variant_int(10);  // Untested.
std::variant<int, std::string> test_variant_string(test_string);  // Untested.

TVector<int> test_tvector = {1, 2, 3, 4};
THashMap<int, int> test_hashmap = {{1, 2}, {3, 4}};
THashSet<int> test_hashset = {1, 2, 3};
THashMultiMap<int, int> test_hashmultimap = {{1, 2}, {3, 4}, {1, 4}};
THashMap<TString, TString> test_tstring_hashmap = {{"Это", "тест"}};

TString test_tstring = "Это тест.";
TString test_nullbyte_tstring{"\x00тест"sv};
TString test_binary_tstring{"\xff\x00\xff\x00"sv};
TUtf16String test_tutf16string = u"Это тест.";
TUtf32String test_tutf32string = U"Это тест.";  // Untested.

TStringBuf test_tstringbuf = "Это тест.";

using TVariantType = std::variant<int, TString>;

TVariantType test_tvariant_default;
TVariantType test_tvariant_int(10);
TVariantType test_tvariant_string(test_tstring);

struct TBadMovable {
    TBadMovable() = default;

    TBadMovable(TBadMovable&&) {
        throw std::exception{};
    }

    TBadMovable& operator=(TBadMovable&&) {
        Y_ABORT("Should not be assigned");
    }
};

using TVariantType2 = std::variant<int, TBadMovable>;

TVariantType2 MakeValuelessVariant() {
    TVariantType2 var;
    try {
        var = TBadMovable{};
    } catch (const std::exception&) {
        Y_ABORT_UNLESS(var.valueless_by_exception());
        return var;
    }
    Y_ABORT("Move-assignment should throw");
}

TVariantType2 test_valueless_variant = MakeValuelessVariant();

using TMaybeType = TMaybe<int>;

TMaybeType test_maybe(465);
TMaybeType test_maybe_empty;

enum TEnumType: int {
    A = 0,
    B = 1,
    SIZE = 2
};

TEnumBitSet<TEnumType, TEnumType::A, TEnumType::SIZE> test_enumbitset = {A, B};

int test_int = 1;
THolder<int> test_holder_int(&test_int);
THolder<int> test_holder_empty;
TCowPtr<TSimpleSharedPtr<int>> test_cow_shared_empty;
TCowPtr<TSimpleSharedPtr<int>> test_cow_shared{new int(1)};

THolder<std::string> test_holder_string_empty;

std::map<int, int>::iterator test_map_iterator;
std::multimap<int, int>::iterator test_multimap_iterator;
std::set<int>::iterator test_set_iterator;
std::deque<int>::iterator test_deque_iterator;
std::unordered_map<int, int>::iterator test_unordered_map_iterator;
std::unordered_set<int>::iterator test_unordered_set_iterator;
std::optional<int> test_optional_int_empty;
std::optional<int> test_optional_int(123);

std::atomic<int> test_atomic_int(12);
std::atomic<std::array<int64_t, 3>> test_atomic_array(std::array<int64_t, 3>{1, 2, 3});

std::list<int> test_list_empty;
std::list<int> test_list{1, 2, 3};
std::forward_list<int> test_forward_list_empty;
std::forward_list<int> test_forward_list{1, 2, 3};

formats::json::Value test_json = formats::json::FromString(
    R"({"a":[1,{},[]],"b":[true,false],"c":{"internal":{"subkey":2}},"i":-1,"u":1,"i64":-18446744073709551614,"u64":18446744073709551614,"d":0.4})");
formats::json::Value test_json_empty;

// Variable which can't be statically initialized due to undetermined order
// of static initialization in C++.
void init() {
    test_map_iterator = test_map.begin();
    test_multimap_iterator = test_multimap.begin();
    test_set_iterator = test_set.begin();
    test_deque_iterator = test_deque.begin();
    test_unordered_map_iterator = test_unordered_map.begin();
    test_unordered_set_iterator = test_unordered_set.begin();
}

void stop_here() {
    [[maybe_unused]] volatile int dummy;
    dummy = 0;
}

int main() {
    init();
    stop_here();
}
