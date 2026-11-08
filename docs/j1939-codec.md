# J1939 の型付きメッセージ（Codec）

PG の信号を YAML で定義すると、ビルドのときに C++ の型ができます。アプリはビット位置やスケーリングを書かずに、信号を値として読み書きします。送受信はこれまでどおりノードと部品（`PeriodicSender`、`Monitor` など）が行い、Codec はバイト列との変換だけを受け持ちます。API はまだ変わることがあります。

```cpp
codec::attach_sender<EngineStatus>(ecu, 20ms, [&](EngineStatus& m) { m.speed.set(engine.rpm); });
codec::on_message<SpeedRequest>(ecu, [&](const SpeedRequest& m) {
    if (const auto rpm = m.speed.value()) { engine.requested = static_cast<std::uint16_t>(*rpm); }   // "not available" なら値はない
});
```

完全な例は [examples/j1939/ecu_pair](../examples/j1939/ecu_pair)（`ecu_pair_messages.yaml` と `ecu_pair.hpp`）です。

## 組み込み方

```cmake
vehicle_sil_add_j1939_codec(engine_messages engine_messages.yaml)   # engine_messages.hpp ができる
target_link_libraries(my_ecu PRIVATE engine_messages)
```

- `vehicle_sil_add_j1939_codec(<target> <定義.yaml>...)` は、定義ごとにヘッダー（`<ファイル名から最後の拡張子を除いたもの>.hpp`。`engine.v2.yaml` なら `engine.v2.hpp`）をビルドのときに生成し、それを include パスに置く INTERFACE ライブラリ `<target>` を作ります。`<target>` は `vehicle_sil::j1939_codec` をリンクします。定義か生成器を変えると、ヘッダーを作り直します。
- 生成には Python 3.8 以上と PyYAML が要ります（この関数を呼んだときだけ）。生成したヘッダーは `vehicle_sil/j1939/codec.hpp` だけを include します。
- 生成器は `tools/j1939_codegen/j1939_codegen.py <定義.yaml> -o <ヘッダー>` で直接も動きます。

## 定義の書き方

```yaml
namespace: engine::messages        # 生成する型の名前空間
indicators: j1939                  # unsigned の信号の既定の特別な値（j1939 か none。既定は j1939）
messages:
  - name: EngineStatus
    comment: Engine -> all, every 20 ms.
    proprietary_b: 0x10            # PGN 0xFF10
    priority: 6
    signals:
      - name: speed
        start_bit: 0
        length: 16
        scale: 0.125               # 1/8 とも書ける
        unit: rpm
      - name: state
        start_bit: 16
        length: 2
        values: {stopped: 0, running: 1}
```

### メッセージ

| キー | 意味 |
|---|---|
| `name` | 型の名前（C++ の識別子） |
| `pgn` | PGN（18 ビット。PDU1 は下位 8 ビットが 0） |
| `proprietary_a` | Proprietary A（`{}` で PGN 0xEF00、`{data_page: 1}` で 0x1EF00）。PDU1 なので、送るときに宛先を指定する |
| `proprietary_b` | Proprietary B のグループ拡張（`0x10` で PGN 0xFF10）。`{group_extension: 0x10, data_page: 1}` で 0x1FF10 |
| `priority` | 優先度 0〜7（既定 6）。`codec::send()` と `codec::attach_sender()` はこの優先度で送る |
| `length` | バイト数 1〜1785（既定 8）。9 以上はトランスポートプロトコルで送る |
| `fill` | 信号のないビットの値（既定 0xFF。1 バイトの値を各バイトに使う） |
| `selector` | 同じ PGN の別のメッセージと区別する定数のフィールド（`{start_bit, length, value}`、`byte_order` も可） |
| `signals` | 信号の並び（なくてもよい） |
| `comment` | 型のコメントになる |

`pgn`、`proprietary_a`、`proprietary_b` のどれか 1 つを書きます。

### 信号

| キー | 意味 |
|---|---|
| `name` | メンバーの名前（C++ の識別子） |
| `start_bit`、`length` | 位置と長さ（1〜64 ビット。下の「ビットの位置」） |
| `byte_order` | `little`（既定）か `big` |
| `signed` | `true` で 2 の補数（既定 `false`） |
| `scale`、`offset` | 物理値 = raw × `scale` + `offset`（既定 1 と 0、`scale` は正の数）。数のほかに `"1/8"`、`"1e-3"` のような文字列も書ける |
| `min`、`max` | 物理値の有効範囲。raw の有効範囲はこの範囲に入る raw 値（端は切り上げと切り捨て）に狭まる |
| `unit` | 単位（文字列） |
| `indicators` | `j1939` か `none`（既定はファイルの `indicators`）。下の「特別な値」 |
| `not_available`、`error` | 特別な値を直接書く（raw のビットパターンの値 1 つか `[first, last]`。2 つの範囲は重ねられない）。書くと `indicators` は使わない。有効範囲は、端にある特別な値の分だけ狭まる（signed では符号付きの値で比べる）。有効範囲の内側に特別な値があれば誤りで、`min` と `max` で外す |
| `values` | 列挙（名前と raw 値）。scoped enum の型を作る。unsigned だけで、`scale`、`offset`、`min`、`max`、`unit` と一緒には書けない |
| `spn` | SPN の番号（読む人のための値。Codec は使わない） |
| `comment` | メンバーのコメントになる |

生成器は次のことを確かめ、違反があれば行番号か場所を示して失敗します。

- 信号がメッセージに収まり、信号どうしとセレクターが重ならないこと。
- 名前が C++ の識別子（キーワードと予約された名前を除く）で、互いに重ならないこと。生成する型が使う名前は、信号の名前に使えません（`encode`、`decode`、`bytes`、`for_each_signal`、`kPgn`、`kPriority`、`kLength`、`kFill`、`Signals`、`Selector`、`output_bytes`、`input_bytes`、`visitor`、`Visitor`、信号の定義のメンバー `layout`、`name`、`unit`、`spn`、`enumerators`、メッセージ自身の名前）。メッセージの名前にも、生成する型が使う名前（`Signals`、`Selector`、`Visitor` など、`layout` 以下を除いた上の名前）は使えません。生成したコードは `std` と `vehicle_sil` を `::` から書くので、それ以外の名前（`std`、`data`、`size_t` など）は使えます。
- 1 つの定義ファイルの中で同じ PGN のメッセージが複数あれば、すべてがセレクターを持ち、その位置が同じで値が違うこと（1 つのフレームが複数の型で decode されないように）。
- 知らないキーと重複したキーがないこと。マージキー（`<<: *base`）で取り込んだキーは上書きできます。

YAML は次のように読みます。`on`、`off`、`yes`、`no` は文字列です（列挙の名前に使えます）。整数は 10 進（先頭に 0 を付けない）、16 進（`0x`）、2 進（`0b`）だけで、`010`、`1:10`、`1:30.0` は誤りです（YAML 1.1 では 8 進と 60 進になるため）。`double` に収まらない数も誤りです。

### ビットの位置

位置はメッセージの先頭から数えます。バイト n のビット b（b = 0 が最下位）が位置 n × 8 + b です。

- `little`: `start_bit` は最下位ビットの位置です。上位のビットは位置を 1 つずつ増やした所にあります（バイトの上位へ、続いて次のバイトへ）。
- `big`: `start_bit` は最上位ビットの位置です。下位のビットはバイトの中で位置を 1 つずつ減らし、バイトのビット 0 の次は次のバイトのビット 7 です（DBC ファイルの Motorola と同じ数え方）。

1 から数える「バイト n、ビット m」の書き方なら、`start_bit` = (n − 1) × 8 + (m − 1) です。

### 特別な値（`indicators`）

`indicators: j1939` の unsigned の信号は、raw の範囲の上の方を特別な値にします。

| 長さ | 有効 | 範囲外 | Error | Not Available |
|---|---|---|---|---|
| 8 の倍数（8〜64 ビット） | 最上位バイトが 0x00〜0xFA | 最上位バイトが 0xFB〜0xFD | 最上位バイトが 0xFE | 最上位バイトが 0xFF |
| 2〜7、9 以上で 8 の倍数でないもの | 0 〜 全ビット 1 − 2 | — | 全ビット 1 − 1 | 全ビット 1 |
| 1 ビット | 0 と 1 | — | — | — |

- signed の信号と `indicators: none` には特別な値がありません（signed で要るときは `not_available` と `error` で書きます。範囲は raw のビットパターンで比べます）。
- 列挙の値は、Error と Not Available の範囲に入れられません。
- `<windows.h>` を `NOMINMAX` なしで include すると、`min` と `max` のマクロが `Signal::min()` の呼び出しを壊します（`(S::min)()` と書くか、`NOMINMAX` を定義します）。同じ理由で、`ERROR`、`IN`、`OUT` など Windows のマクロと同じ名前は避けます。

## 生成される型

上の定義から、次の形の型ができます（要点だけ。実際のコードは名前を `::std::`、`::vehicle_sil::` から書きます）。

```cpp
struct EngineStatus
{
    static constexpr j1939::Pgn kPgn{0xFF10u};
    static constexpr j1939::Priority kPriority = 6;
    static constexpr std::size_t kLength = 8;
    static constexpr std::uint8_t kFill = 0xFF;
    enum class State : std::uint8_t { stopped = 0, running = 1 };
    struct Signals { struct speed { static constexpr codec::SignalLayout layout{...}; ... }; ... };

    codec::Signal<Signals::speed> speed;
    codec::EnumSignal<Signals::state, State> state;

    std::size_t encode(std::span<std::uint8_t> output_bytes) const;     // kLength バイトを書く
    static std::optional<EngineStatus> decode(std::span<const std::uint8_t> input_bytes);
    std::array<std::uint8_t, kLength> bytes() const;
    template <typename Visitor> void for_each_signal(Visitor&& visitor); // 各信号で visitor(signal)（const 版もある）
    friend bool operator==(const EngineStatus&, const EngineStatus&) = default;
};
```

- 新しいメッセージの信号は Not Available です（Not Available のない信号は raw 0）。
- `encode(out)` は、信号のないビットを `kFill` にして `kLength` バイトを書き、`kLength` を返します。`out` が短ければ `std::length_error` です。
- `decode(data)` は、`data` が `kLength` より短いか、セレクターの値が違えば空を返します。長い分は無視します。
- 型は状態を持たず、`encode` と `decode` はメモリを確保しません。
- `Signal` と `EnumSignal` は数値や列挙に暗黙に変換されず、数値も代入できません。

### 信号の API（`vehicle_sil/j1939/codec.hpp`）

| | 意味 |
|---|---|
| `state()` | `Valid`、`OutOfRange`（有効範囲の外。予約の値と、列挙にない値を含む）、`Error`、`NotAvailable`。判定は Not Available、Error、範囲の順 |
| `value()` | `Valid` のときだけ値（`Signal` は物理値の `std::optional<double>`、`EnumSignal` は列挙）。それ以外は空 |
| `value_or(x)` | `value()` が空なら `x` |
| `set(physical)` | 物理値を最も近い raw 値にする（ちょうど半分は 0 から遠い方）。丸めた値が有効範囲の外なら近い端にして `false`（64 ビットの信号で、`double` にすると端と同じになる値は端にして `true`）。NaN は Not Available にして `false`（Not Available のない信号は変えずに `false`） |
| `set(enumerator)` | `EnumSignal` の値 |
| `raw()`、`set_raw(raw)` | raw 値（signed の信号は符号付き）。`set_raw` は信号のビットに入る値なら何でも設定できる（特別な値や範囲外も。試験用）。入らなければ `std::out_of_range` |
| `bits()` | メッセージの中のビット列（unsigned） |
| `set_not_available()`、`set_error()` | Not Available の範囲の最後の値（`j1939` では全ビット 1）、Error の範囲の最初の値（`j1939` では 0xFE、0xFE00 …）。その範囲がない信号ではコンパイルエラー |
| `min()`、`max()`、`resolution()`、`to_physical(raw)` | 有効な物理値の範囲、raw 1 あたりの物理値、raw から物理値（`Signal` だけ。`resolution()` は constexpr） |
| `raw_min`、`raw_max`、`lowest_raw()`、`highest_raw()` | 有効な raw 値の範囲と、ビットに入る raw 値の範囲 |
| `layout`、`name`、`unit`、`spn` | 定義（constexpr） |

物理値は `double` で計算します。raw × scale を `double` に丸めてから offset を足すので、コンパイラーや最適化が掛け算と足し算を 1 回の丸めにまとめることはなく、IEEE の倍精度で計算する環境（x86-64 の SSE、AArch64 など）ではどこでも同じ値になります（拡張精度の x87 で計算する 32 ビットの x86 は対象外です）。53 ビットを超える raw 値の物理値は丸められます（raw 値は正確です）。

## ノードで使う（`vehicle_sil/j1939/codec_node.hpp`）

| 関数 | 意味 |
|---|---|
| `codec::attach_sender<M>(node, period, fill, options)` | `PeriodicSender` を付ける。送るたびに新しい `M`（Not Available のある信号は Not Available、ない信号は raw 0）を作り、`fill(M&)` で信号を設定して送る。優先度は `M::kPriority` で、`options.priority` は既定値（6）のままか `M::kPriority` にする（違えば `std::invalid_argument`）。バッファは `M::kLength` 以上にする |
| `codec::encoder<M>(fill)` | 同じことをする `PeriodicSender::Encoder`（自分で `PeriodicSender` を作るとき。`max_length` は `M::kLength` 以上にする） |
| `codec::on_message<M>(node, handler)` | `M` の PGN を受けて decode できたものだけを `handler(const M&)` か `handler(const M&, const Message&)` に渡す。短いものとセレクターの違うものは渡さない（見るときは `node.on_message()` と `M::decode()`）。戻り値の `HandlerId` で解除する |
| `codec::send(node, message, destination)` | `node.send(M::kPgn, message.bytes(), destination, M::kPriority)` |

- `Monitor` の最新の受信は `M::decode(monitor.data())`、`Requester` の応答は `M::decode(result.data)` で読みます（まだ受けていなければ空）。
- 同じ PGN の複数の型（Proprietary A をセレクターで分けたもの）は、型ごとに `on_message<M>` を登録します。
- `fill` とハンドラは、ほかのハンドラと同じくステップの中で呼ばれます。投げた例外は `Network` を失敗状態にします。

## 試験で使う

- 境界値と特別な値は `set_raw()`、`set_error()`、`set_not_available()` で作り、`state()` と `value()` で確かめます。範囲は `raw_min`、`raw_max`、`min()`、`max()` で引けます。
- `for_each_signal` で、メッセージのすべての信号に同じ確認をできます（`tests/j1939/test_j1939_codec.cpp` は、16 ビット以下の信号のすべての raw 値で encode と decode の往復を確かめています）。
- 壊れたメッセージ（短い、セレクターが違う）は `M::decode()` が空を返します。

## 手で書く型

生成器を使わずに、同じ形の型を C++ で書けます。`codec::MessageType` の要件（`kPgn`、`kPriority`、`kLength`、`kFill`、`for_each_signal`、`encode`、`decode`）を満たし、`encode`、`decode`、`bytes` は `codec::encode_message`、`codec::decode_message`、`codec::message_bytes` に任せます。信号の `Spec` は `layout`、`name`、`unit`（と、任意で `spn`、列挙なら `enumerators`）を持つ型です。有効範囲、特別な値の範囲、列挙の値が信号のビットに収まること、信号がメッセージの長さに収まることは、コンパイルのときに確かめます。特別な値と有効範囲の重なり、同じ PGN の型の区別は確かめないので、生成器の規則に合わせて書きます。

## 制限

- マルチプレクス（セレクターの値で配置が変わる信号）、可変長のデータ（文字列、`*` で区切るフィールド）、配列と繰り返し（DM1 の DTC など）は扱いません。
- SAE J1939 の PGN と SPN の定義（J1939DA）は同梱していません。標準の PG を使うときは、利用者が入手した定義から YAML を書きます。

## 規格との一致を確かめていない点

- `indicators: j1939` の範囲（上の表）と、`set_error()` が Error の範囲の最初の値を使うこと。広く使われている慣行に合わせたもので、規格の本文とは照合していません。
