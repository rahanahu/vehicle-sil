# J1939 の型付きメッセージ（Codec）

PG の信号を YAML で定義して C++ の型を生成し、信号を値として読み書きする方法と、定義のキーと生成される型の API の一覧です。API はまだ変わることがあります。

YAML で定義すると、ビルドのときに C++ の型ができます。アプリはビット位置やスケーリングを書かずに、信号を値として読み書きします。送受信はこれまでどおりノードと部品（`PeriodicSender`、`Monitor` など）が行い、Codec はバイト列との変換だけを受け持ちます。

| したいこと | 使うもの | 節 |
|---|---|---|
| 定義から型を生成してリンクする | `vehicle_sil_add_j1939_codec()` | [CMake に組み込む](#cmake-に組み込む) |
| メッセージと信号を定義する | YAML のキー | [定義の書き方](#定義の書き方) |
| 信号の位置を数える | `start_bit`、`byte_order` | [ビットの位置](#ビットの位置) |
| Error や Not Available のような値を定義する | `indicators`、`special_values` | [特別な値（`indicators`）](#特別な値indicators) |
| 信号のないビットの値を決める | `fill` | [未使用のビットの埋め値（`fill`）](#未使用のビットの埋め値fill) |
| 信号を読み書きする | `Signal`、`EnumSignal` | [信号の API（`vehicle_sil/j1939/codec.hpp`）](#信号の-apivehicle_silj1939codechpp) |
| 型でメッセージを送受信する | `codec::attach_sender`、`codec::on_message` など | [ノードで使う（`vehicle_sil/j1939/codec_node.hpp`）](#ノードで使うvehicle_silj1939codec_nodehpp) |
| 境界値と特別な値をテストする | `set_raw()`、`for_each_signal` | [テストで使う](#テストで使う) |
| 生成器を使わずに型を書く | `codec::MessageType` | [手で書く型](#手で書く型) |

[examples/j1939/ecu_pair/ecu_pair.hpp](../examples/j1939/ecu_pair/ecu_pair.hpp) の `install_engine()` の一部です（`node` はノード、`engine` はモデル）。メッセージの定義は、同じディレクトリの `ecu_pair_messages.yaml` です。

```cpp
    codec::attach_sender<EngineStatus>(node, 20ms, [&engine](EngineStatus& m) {
        m.speed.set(engine.rpm);
        m.state.set(engine.rpm > 0 ? EngineStatus::State::running : EngineStatus::State::stopped);
    });
    codec::on_message<SpeedRequest>(node, [&engine](const SpeedRequest& m) {
        if (const auto rpm = m.speed.value()) // not when the cab sends "not available"
        {
            engine.requested = static_cast<std::uint16_t>(*rpm);
        }
    });
```

YAML で定義して使う、単独で動く例は [examples/typed_messages](../examples/typed_messages) です。

## CMake に組み込む

`engine_messages` と `my_ecu` は、自分のターゲットの名前に置き換えます。リポジトリでの使い方は [examples/typed_messages/CMakeLists.txt](../examples/typed_messages/CMakeLists.txt) にあります。

```cmake
vehicle_sil_add_j1939_codec(engine_messages engine_messages.yaml)   # engine_messages.hpp ができる
target_link_libraries(my_ecu PRIVATE engine_messages)
```

`vehicle_sil_add_j1939_codec(<target> <定義.yaml>...)` は、次のことをします。

| 項目 | 内容 |
|---|---|
| 生成するもの | 定義ごとのヘッダーを、ビルドのときに生成します。 |
| ヘッダーの名前 | `<ファイル名から最後の拡張子を除いたもの>.hpp` です（`engine.v2.yaml` なら `engine.v2.hpp`）。 |
| `<target>` | 生成したヘッダーを include パスに置く INTERFACE ライブラリで、`vehicle_sil::j1939_codec` をリンクします。 |
| 作り直すとき | 定義か生成器を変えると、ヘッダーを作り直します。 |
| 生成したヘッダーの include | `vehicle_sil/j1939/codec.hpp` だけです。 |

生成には Python 3.11 以上と PyYAML が必要です（この関数を呼んだときだけ）。使う Python は次の順に決まります（[依存ライブラリ](dependencies.md)）。

1. vehicle-sil を最上位のプロジェクトとしてビルドするときは、`uv sync` で作ったリポジトリの `.venv`
2. それがなければ、`find_package(Python3)` で見つかる Python

生成器は、次のコマンドで直接実行することもできます。

```sh
tools/j1939_codegen/j1939_codegen.py <定義.yaml> -o <ヘッダー>
```

## 定義の書き方

`vehicle_sil_add_j1939_codec()` に渡せる、1 つの定義ファイルの全体です。

```yaml
namespace: engine::messages        # 生成する型の名前空間
indicators: j1939                  # 特別な値のプロファイル（全体の既定。書かなければ none）
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

### ファイルの最上位

| キー | 意味 |
|---|---|
| `namespace` | 生成する型の名前空間 |
| `messages` | メッセージの並び |
| `indicators` | 全体の既定の、特別な値のプロファイル |
| `indicator_profiles` | プロファイルの定義 |
| `fill` | 全体の既定の埋め値（1 バイト） |

### メッセージ

| キー | 意味 |
|---|---|
| `name` | 型の名前（C++ の識別子） |
| `pgn` | PGN（18 ビット。PDU1 は下位 8 ビットが 0） |
| `proprietary_a` | Proprietary A（`{}` で PGN 0xEF00、`{data_page: 1}` で 0x1EF00）。PDU1 なので、送信するときに宛先を指定する |
| `proprietary_b` | Proprietary B のグループ拡張（`0x10` で PGN 0xFF10、`{group_extension: 0x10, data_page: 1}` で 0x1FF10） |
| `priority` | 優先度 0〜7（既定 6）。`codec::send()` と `codec::attach_sender()` はこの優先度で送信する |
| `length` | バイト数 1〜1785（既定 8）。9 以上はトランスポートプロトコルで送信する |
| `fill` | 信号のないビットの値。1 バイトの値（各バイトに使う）か、メッセージの長さのバイト列（既定はファイルの `fill`、それもなければ 0xFF） |
| `indicators` | このメッセージの信号の、特別な値のプロファイル（既定はファイルの `indicators`）。下の「特別な値」 |
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
| `min`、`max` | 物理値の有効範囲。raw の有効範囲は、この範囲に入る raw 値（端は切り上げと切り捨て）に狭まる |
| `unit` | 単位（文字列） |
| `indicators` | この信号の、特別な値のプロファイル（既定はメッセージの `indicators`）。下の「特別な値」 |
| `special_values` | この信号だけの特別な値（名前と値。書き方はプロファイルの `values` と同じ）。継承したプロファイルを置き換える |
| `not_available`、`error` | `special_values` の `not_available`、`error` の短い書き方 |
| `values` | 名前と raw 値の列挙で、scoped enum の型を作る（unsigned だけ。`scale`、`offset`、`min`、`max`、`unit` と一緒には書けない） |
| `spn` | SPN の番号（読む人のための値。Codec は使わない） |
| `comment` | メンバーのコメントになる |

### 生成器が確認すること

生成器は次のことを確認し、違反があれば行番号か場所を示して失敗します。

| 対象 | 確認すること |
|---|---|
| 信号の位置 | 信号がメッセージに収まり、信号どうしとセレクターが重ならない。 |
| 名前 | C++ の識別子（キーワードと予約された名前を除く）で、互いに重ならない。 |
| 同じ PGN のメッセージ | 1 つの定義ファイルの中に複数あれば、すべてがセレクターを持ち、その位置が同じで値が違う（1 つのフレームが複数の型で decode されないように）。 |
| キー | 知らないキーと重複したキーがない。 |
| 特別な値 | 信号のビットに収まり、互いに重ならず、有効範囲の内側にない（下の「特別な値」）。 |

マージキー（`<<: *base`）で取り込んだキーは上書きできます。

### 使えない名前

生成する型が使う名前は、信号の名前とメッセージの名前に使えません。

| 名前 | 信号の名前 | メッセージの名前 |
|---|---|---|
| `encode`、`decode`、`bytes`、`for_each_signal`、`kPgn`、`kPriority`、`kLength`、`kFill`、`Signals`、`Selector`、`output_bytes`、`input_bytes`、`visitor`、`Visitor`、`kFillBytes` | 使えない | 使えない |
| 信号の定義のメンバー `layout`、`name`、`unit`、`spn`、`enumerators`、`special_values` | 使えない | 使える |
| メッセージ自身の名前 | 使えない | — |

生成したコードは `std` と `vehicle_sil` を `::` から書くので、それ以外の名前（`std`、`data`、`size_t` など）は使えます。

### YAML の値の読み方

| 書いたもの | 読み方 |
|---|---|
| `on`、`off`、`yes`、`no` | 文字列です（列挙の名前に使えます）。 |
| 整数 | 10 進（先頭に 0 を付けない）、16 進（`0x`）、2 進（`0b`）だけです。 |
| `010`、`1:10`、`1:30.0` | エラーです（YAML 1.1 では 8 進と 60 進になるため）。 |
| `double` に収まらない数 | エラーです。 |

### ビットの位置

位置はメッセージの先頭から数えます。バイト n のビット b（b = 0 が最下位）が位置 n × 8 + b です。

| `byte_order` | `start_bit` が指すもの | 残りのビットの並び |
|---|---|---|
| `little` | 最下位ビットの位置 | 上位のビットは位置を 1 つずつ増やした所にある（バイトの上位へ、続いて次のバイトへ） |
| `big` | 最上位ビットの位置 | 下位のビットはバイトの中で位置を 1 つずつ減らし、バイトのビット 0 の次は次のバイトのビット 7（DBC ファイルの Motorola と同じ数え方） |

1 から数える「バイト n、ビット m」の書き方なら、`start_bit` = (n − 1) × 8 + (m − 1) です。

### 特別な値（`indicators`）

特別な値は、raw 値の一部に「Error」「Not Available」「校正中」のような意味を持たせるものです。特別な値は信号ごとに決まり、Codec 全体で予約する値はありません。特別な値のない信号では、すべてのビットパターンが値です。

特別な値は、名前と raw 値（1 つか範囲）の組です。

| 項目 | ルール |
|---|---|
| 名前 | 小文字、数字、`_` で書きます。 |
| `error`、`not_available` という名前 | 状態 `Error`、`NotAvailable` になります。 |
| それ以外の名前 | 状態 `Special` になり、`special()` で名前を引けます。 |
| 値 | raw 値で書きます。 |
| signed の信号の値 | 負の数でも、ビットパターンでも書けます（8 ビットの signed で `-1` と `0xFF` は同じ値）。 |
| 範囲 `[first, last]` | raw 値で `first <= last` です（signed で符号をまたぐ範囲は、2 つの特別な値に分けます）。 |
| 列挙の値 | 特別な値と重ねられません。 |

送信する値は、`set_special(名前)` が書く値です。`not_available` なら、新しいメッセージの初期値にもなります。

| 書き方 | 送信する値 |
|---|---|
| 値が 1 つ | その値 |
| 範囲 | 範囲の最初の値 |
| `{range: [first, last], send: v}` | `v` |

組み込みの `j1939` の `not_available` は、`send` で全ビット 1 を送信します。範囲で書いた `not_available` で同じく全ビット 1 を送信するときは、`send` を書きます。

有効範囲（`raw_min`〜`raw_max`）は、端にある特別な値の分だけ狭まります。このため、物理値を設定して特別な値になることはありません。有効範囲の内側に特別な値があればエラーなので、`min` と `max` で外します。

列挙の信号では、特別な値でも列挙子でもない値はすべて `Undefined` です（列挙には範囲がないので、`OutOfRange` にはなりません）。

#### プロファイル

特別な値の組に名前を付けたものがプロファイルです。`indicator_profiles` に一度定義し、名前で選びます。定義ファイルの最上位に書く部分です。

```yaml
indicator_profiles:
  vendor:                                  # 名前は自由
    - when: {length: 8, signed: false}     # この規則は 8 ビットの unsigned の信号に
      values: {sensor_error: 0xFE, not_available: 0xFF}
    - when: {length: 16, signed: false}
      values:
        sensor_error: [0xFE00, 0xFEFF]     # 範囲
        not_available: {range: [0xFF00, 0xFFFF], send: 0xFFFF}   # 全ビット 1 を送る（既定は 0xFF00）
    - when: {length: 16, signed: true}
      values: {sensor_error: -32768}       # signed は負の数でも書ける（0x8000 と同じ）
  calibration:                             # 規則が 1 つなら、並びにしなくてよい
    when: {length: 8, signed: false}
    values: {calibrating: 0, not_available: 0xFF}
```

プロファイルは規則の並びです。

| 項目 | ルール |
|---|---|
| 規則が持つもの | `values`（特別な値）と、任意の `when` です。 |
| `when` | `length`（長さか長さの並び）と `signed`（true か false）で、規則を適用する信号を限定します。 |
| 信号に使う規則 | `when` が合う最初の規則です。 |
| `when` のない規則 | どの信号にも合います（値が信号のビットに入らない、または有効範囲の内側に来る信号があればエラー）。 |
| 組み込みのプロファイル | `none`（特別な値なし）と `j1939`（下の表）です。 |
| ファイルで `j1939` を定義したとき | 組み込みのものを置き換えます。 |
| `none` | 定義できません。 |

ファイルやメッセージ全体に適用するプロファイルでは、規則ごとに `length` と `signed` を書くのが安全です。signed の信号では `0xFF` が −1 になり、有効範囲の内側に来るためです。

#### 継承と上書き

信号の特別な値は、次の順に最初に見つかったもので決まります。継承元と継承先の特別な値は混ぜず、見つかったもので丸ごと置き換えます。どの信号がどの特別な値を持つかは、生成のときに決まります。

1. 信号の `special_values`（と `not_available`、`error`）
2. 信号の `indicators`
3. メッセージの `indicators`
4. ファイルの `indicators`
5. どれもなければ `none`

既定は `none` です。J1939 の特別な値は `indicators: j1939` を書いたときだけ使います（Proprietary B のメッセージに、意図せず J1939 の解釈が適用されないように）。`indicators: none` は、上の階層の指定を打ち消します。

選んだプロファイルに、信号の長さと符号に合う規則がないときの扱いは、選んだ場所で変わります。

| プロファイルを選んだ場所 | 合う規則がないとき |
|---|---|
| 信号の `indicators` | エラーです。 |
| メッセージかファイル（信号は継承する） | その信号は特別な値を持ちません。 |
| メッセージかファイルで、ファイルで定義したプロファイルが、継承する信号のどれにも合わない | エラーです（`when` の書き間違いで、すべての信号から特別な値が黙って消えないように）。 |

- 全体に `j1939` を指定しても、1 ビットのフラグや 12 ビットの値は値のままです。
- 組み込みの `j1939` は、どの信号にも合わないときのエラーの対象外です（表示灯やフラグだけのファイルにも `indicators: j1939` を書けます）。
- 生成器のエラーメッセージは、特別な値の名前、値（16 進）、どこで選んだプロファイルか（例: `profile vendor of the file`）を示します。

#### 組み込みの `j1939` プロファイル

| 信号 | 有効 | `parameter_specific` | `reserved` | `error` | `not_available`（送信する値） |
|---|---|---|---|---|---|
| 8 ビット unsigned | 0x00〜0xFA | 0xFB | 0xFC〜0xFD | 0xFE | 0xFF |
| 16 ビット unsigned | 0x0000〜0xFAFF | 0xFB00〜0xFBFF | 0xFC00〜0xFDFF | 0xFE00〜0xFEFF（0xFE00） | 0xFF00〜0xFFFF（0xFFFF） |
| 32 ビット unsigned | 0x00000000〜0xFAFFFFFF | 0xFB000000〜0xFBFFFFFF | 0xFC000000〜0xFDFFFFFF | 0xFE000000〜0xFEFFFFFF（0xFE000000） | 0xFF000000〜0xFFFFFFFF（0xFFFFFFFF） |
| 2 ビット unsigned | 0、1 | — | — | 2 | 3 |

ほかの長さ（1 ビット、3〜7 ビット、24 ビット、64 ビットなど）と signed の信号には規則がありません。規格での扱いを確認していない長さに、推測した規則は適用しません。必要なときは、ファイルで `j1939` を定義し直すか、信号に `special_values` を書きます。

表の値は広く使われている慣行に合わせたもので、規格の本文とは照合していません（末尾の「規格との一致」）。

#### 標準の J1939 と Proprietary B の使い分け

| ファイルの内容 | 書き方 |
|---|---|
| 標準の PG を定義するファイル | 最上位に `indicators: j1939` を 1 行書き、値域や意味が決まっている SPN は、信号の `min`、`max`、`special_values` で上書きします。 |
| Proprietary B だけのファイル（独自の特別な値なし） | 何も書きません（すべてのビットパターンが値です）。 |
| Proprietary B だけのファイル（メーカー独自の特別な値あり） | プロファイルを 1 つ定義してファイルかメッセージの `indicators` で選び、例外の信号だけに `indicators` か `special_values` を書きます。 |
| 1 つのファイルに混ぜるとき | ファイルの `indicators` をよく使う方にし、もう一方のメッセージに `indicators` を書きます。 |

信号の `special_values` は、継承したプロファイルを丸ごと置き換えます（足し合わせません）。標準の信号で 1 つの値だけを変えるときも、残したい値を一緒に書きます。

メッセージの `signals` の 1 つです。

```yaml
# 8 ビットの標準の信号で、0xFB（parameter_specific）を独自の意味にする
- name: coolant_level
  start_bit: 0
  length: 8
  special_values: {level_unknown: 0xFB, reserved: [0xFC, 0xFD], error: 0xFE, not_available: 0xFF}
```

1 つの定義ファイルの全体です。

```yaml
# 数百の信号を持つ Proprietary B のファイル
namespace: vendor::messages
indicators: vendor                 # 全部の信号に
fill: 0x00                         # 全部のメッセージの未使用のビット
indicator_profiles:
  vendor:
    - when: {length: 8, signed: false}
      values: {sensor_error: 0xFE, not_available: 0xFF}
    - when: {length: 16, signed: false}
      values: {sensor_error: [0xFE00, 0xFEFF], not_available: {range: [0xFF00, 0xFFFF], send: 0xFFFF}}
messages:
  - name: Pressures
    proprietary_b: 0x40
    signals:
      - {name: p1, start_bit: 0, length: 8, scale: 0.5, unit: kPa}
      - {name: p2, start_bit: 8, length: 8, scale: 0.5, unit: kPa}
      - {name: raw_count, start_bit: 16, length: 16, indicators: none}            # すべての値を使う
      - {name: level, start_bit: 32, length: 8, special_values: {calibrating: 0}} # この信号だけ別
  - name: Debug
    proprietary_b: 0x41
    indicators: none                                                               # このメッセージは特別な値なし
    signals:
      - {name: word, start_bit: 0, length: 32}
```

### 未使用のビットの埋め値（`fill`）

信号もセレクターもないビットを埋める値です。特別な値とは別のもので、信号の値は変えません。

| 書く場所 | 書けるもの |
|---|---|
| ファイルの `fill` | 1 バイトの値で、全メッセージの既定になります。 |
| メッセージの `fill` | ファイルの既定を上書きします。 |
| メッセージの `fill`（バイト列） | 長さぶんのバイト列（`[0xFF, 0x00, ...]`）で、ビットパターンを指定できます。 |
| どれもないとき | 0xFF です。 |

### Windows のマクロと名前の衝突

`<windows.h>` を `NOMINMAX` なしで include すると、`min` と `max` のマクロが `Signal::min()` の呼び出しを壊します（`(S::min)()` と書くか、`NOMINMAX` を定義します）。同じ理由で、`ERROR`、`IN`、`OUT` など Windows のマクロと同じ名前は避けます。

## 生成される型

「定義の書き方」の最初の定義から、次の形の型ができます。要点だけを示したもので、このままではコンパイルできません（`...` は省いた部分。実際のコードは名前を `::std::`、`::vehicle_sil::` から書きます）。

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

| 項目 | 動作 |
|---|---|
| 新しいメッセージの信号 | Not Available です（`not_available` のない信号は raw 0）。 |
| `encode(out)` | 信号のないビットを `kFill`（バイト列の埋め値では `kFillBytes`）にして `kLength` バイトを書き、`kLength` を返します。 |
| `encode(out)` で `out` が短いとき | `std::length_error` です。 |
| `decode(data)` | `data` が `kLength` より短いか、セレクターの値が違えば空を返します（長い分は無視します）。 |
| 状態とメモリ | 型は状態を持たず、`encode` と `decode` はメモリを確保しません。 |
| `Signal` と `EnumSignal` | 数値や列挙に暗黙に変換されず、数値も代入できません。 |

### 信号の API（`vehicle_sil/j1939/codec.hpp`）

| | 意味 |
|---|---|
| `state()` | 信号の状態（下の表）。判定は特別な値が先で、それから範囲か列挙子 |
| `special()` | 持っている特別な値の名前（`std::optional<std::string_view>`）。特別な値でなければ空 |
| `value()` | `Valid` のときだけ値（`Signal` は物理値の `std::optional<double>`、`EnumSignal` は列挙）。それ以外は空 |
| `value_or(x)` | `value()` が空なら `x` |
| `set(physical)` | 物理値を最も近い raw 値にする（下の表） |
| `set(enumerator)` | `EnumSignal` の値 |
| `raw()` | raw 値（signed の信号は符号付き）。特別な値、範囲外、未定義の値でも、受信した raw 値はそのまま残る |
| `set_raw(raw)` | 信号のビットに入る値なら何でも設定でき、特別な値の制限を受けない。入らなければ `std::out_of_range` |
| `bits()` | メッセージの中のビット列（unsigned） |
| `set_special(name)` | 特別な値をその名前で設定する（送信する値を書く）。その名前の特別な値がなければ `std::invalid_argument` |
| `set_not_available()`、`set_error()` | `set_special("not_available")`、`set_special("error")`（その送信する値を書く）。その特別な値がない信号ではコンパイルエラー |
| `has_special(name)`、`specials` | 名前の特別な値があるか（constexpr）、特別な値の定義の配列（`SpecialValue`: `name`、`first`、`last`、`send`） |
| `min()`、`max()`、`resolution()`、`to_physical(raw)` | 有効な物理値の範囲、raw 1 あたりの物理値、raw から物理値（`Signal` だけ。`resolution()` は constexpr） |
| `raw_min`、`raw_max`、`lowest_raw()`、`highest_raw()` | 有効な raw 値の範囲（`EnumSignal` では最小と最大の列挙子）と、ビットに入る raw 値の範囲 |
| `layout`、`name`、`unit`、`spn` | 定義（constexpr） |

| `state()` | なるとき |
|---|---|
| `Valid` | 値として読めるとき |
| `Error`、`NotAvailable` | その名前の特別な値のとき |
| `Special` | そのほかの名前の特別な値のとき |
| `OutOfRange` | `Signal` で、特別な値でなく有効範囲の外のとき |
| `Undefined` | `EnumSignal` で、特別な値でもどの列挙子でもない値のとき |

| `set(physical)` に渡した値 | 設定される値 | 戻り値 |
|---|---|---|
| 有効範囲の中の物理値 | 最も近い raw 値（ちょうど半分は 0 から遠い方） | `true` |
| 丸めた値が有効範囲の外 | 近い端 | `false` |
| 64 ビットの信号で、`double` にすると端と同じになる値 | 端 | `true` |
| NaN | Not Available（Not Available のない信号は変えない） | `false` |

受信した値は、状態で分けて読みます。特別な値でも範囲外でも、`raw()` は受信した値をそのまま返します（範囲で定義した特別な値なら、範囲の中のどの値だったかまでわかります）。

断片です（`m` は受信した型付きメッセージ、`use`、`log`、`detail` は利用者のもの）。特別な値の名前を読む動く例は [examples/typed_messages/main.cpp](../examples/typed_messages/main.cpp) にあります。

```cpp
if (const auto v = m.speed.value()) {
    use(*v);                                        // 物理値
} else if (const auto name = m.speed.special()) {
    log(*name, m.speed.raw());                      // 例: "sensor_error", 0xFE42
    // 範囲の先頭からの差（下位のバイトに詳細を載せる独自の特別な値など）
    for (const auto& s : decltype(m.speed)::specials) {
        if (s.name == *name) { detail = m.speed.raw() - s.first; }
    }
} else {
    log("out of range", m.speed.raw());             // 範囲外（列挙の信号では Undefined）
}
```

物理値は `double` で計算します。raw × scale を `double` に丸めてから offset を足すので、コンパイラーや最適化が掛け算と足し算を 1 回の丸めにまとめることはありません。そのため、IEEE の倍精度で計算する環境（x86-64 の SSE、AArch64 など）ではどこでも同じ値になります（拡張精度の x87 で計算する 32 ビットの x86 は対象外です）。53 ビットを超える raw 値の物理値は丸められます（raw 値は正確です）。

## ノードで使う（`vehicle_sil/j1939/codec_node.hpp`）

| 関数 | 意味 |
|---|---|
| `codec::attach_sender<M>(node, period, fill, options)` | `PeriodicSender` を付け、送信のたびに新しい `M` を作り、`fill(M&)` で信号を設定して送信する |
| `codec::encoder<M>(fill)` | 同じことをする `PeriodicSender::Encoder`（自分で `PeriodicSender` を作るとき。`max_length` は `M::kLength` 以上にする） |
| `codec::on_message<M>(node, handler)` | `M` の PGN を受信して、decode できたものだけを `handler(const M&)` か `handler(const M&, const Message&)` に渡す（戻り値の `HandlerId` で解除する） |
| `codec::send(node, message, destination, when_busy)` | `node.send(M::kPgn, message.bytes(), destination, M::kPriority, when_busy)`（`when_busy` の既定は `WhenBusy::Fail`） |
| `codec::decode<M>(message)` | `message.pgn` が `M::kPgn` のときだけ `M::decode(message.data)`（ほかは空）。いろいろな PGN が来る所（`Exchange` の `match` など）で使う |

`codec::attach_sender` には、次のルールがあります。

| 項目 | ルール |
|---|---|
| 新しい `M` の信号 | Not Available のある信号は Not Available、ない信号は raw 0 です。 |
| 優先度 | `M::kPriority` で送信します。 |
| `options.priority` | 既定値（6）のままか `M::kPriority` にします（違えば `std::invalid_argument`）。 |
| バッファ | `M::kLength` 以上にします。 |

そのほかの使い方です。

| 場面 | 書き方 |
|---|---|
| `on_message<M>` が渡さないもの（短いもの、セレクターの違うもの）を見る | `node.on_message()` と `M::decode()` を使います。 |
| `Monitor` の最新の受信を読む | `M::decode(monitor.data())` で読みます（まだ受信していなければ空）。 |
| `Monitor` で、同じ PGN に別の型が載る | `MonitorOptions::accept` で `codec::decode<M>(message)` が値を返すものだけにします。 |
| `Requester` の応答を読む | `M::decode(result.data)` で読みます。 |
| 同じ PGN の複数の型（Proprietary A をセレクターで分けたもの） | 型ごとに `on_message<M>` を登録します。 |

`fill` とハンドラは、ほかのハンドラと同じくステップの中で呼ばれます。投げた例外は `Network` を失敗状態にします。

## テストで使う

| 確認したいもの | 方法 |
|---|---|
| 境界値と特別な値 | `set_raw()`、`set_special()` で作り、`state()`、`special()`、`value()`、`raw()` で確認します。 |
| 範囲と特別な値の定義 | 範囲は `raw_min`、`raw_max`、`min()`、`max()`、特別な値は `specials` で引けます。 |
| メッセージのすべての信号 | `for_each_signal` で、同じ確認をすべての信号にできます。 |
| 壊れたメッセージ（短い、セレクターが違う） | `M::decode()` が空を返します。 |

`tests/j1939/test_j1939_codec.cpp` は、`for_each_signal` を使って、16 ビット以下の信号のすべての raw 値で encode と decode の往復を確認しています。

## 手で書く型

生成器を使わずに、同じ形の型を C++ で書けます。`codec::MessageType` の要件を満たし、`encode`、`decode`、`bytes` は `codec::encode_message`、`codec::decode_message`、`codec::message_bytes` に任せます。

| 書くもの | 要件 |
|---|---|
| メッセージの型 | `kPgn`、`kPriority`、`kLength`、`kFill` か `kFillBytes`、`for_each_signal`、`encode`、`decode` を持ちます。 |
| `kFill` と `kFillBytes` | どちらか一方だけを持ちます（`kFillBytes` は `std::array<std::uint8_t, kLength>`）。 |
| 信号の `Spec` | `layout`、`name`、`unit`（と、任意で `spn`、`special_values`、列挙なら `enumerators`）を持つ型です。 |

| 確認の時点 | 確認すること |
|---|---|
| コンパイルのとき | 有効範囲、特別な値の範囲と送信する値、列挙の値が信号のビットに収まること、信号がメッセージの長さに収まること |
| 確認しない（生成器のルールに合わせて書く） | 特別な値と有効範囲の重なり、同じ PGN の型の区別 |

## 制限

- マルチプレクス（セレクターの値で配置が変わる信号）、可変長のデータ（文字列、`*` で区切るフィールド）、配列と繰り返し（DM1 の DTC など）は扱いません。
- SAE J1939 の PGN と SPN の定義（J1939DA）は同梱していません。標準の PG を使うときは、利用者が入手した定義から YAML を書きます。

## 規格との一致

規格の本文で確認した事項は、まだありません。組み込みの `j1939` プロファイルは、広く使われている慣行に合わせたものです。確認していない点は次のとおりです。

- 8、16、32 ビットの範囲と、`parameter_specific`（0xFB…）と `reserved`（0xFC〜0xFD…）の区別。
- 2 ビットの範囲（2 が Error、3 が Not Available）。命令の信号でも同じかどうか。
- 規則を設けていない長さ（1 ビット、3〜7 ビット、24 ビット、64 ビット）と、signed の信号の特別な値。
- 送信する値: Error は範囲の最初の値（0xFE00 など）、Not Available は全ビット 1。
- 未使用のビットとバイトの埋め値の既定（0xFF）。

確認した値と違うときは、ファイルで `j1939` プロファイルを定義し直せば、Codec の更新を待たずに使えます（範囲で書く `not_available` に、全ビット 1 を送る `send` を書くのを忘れずに）。

## 関連するドキュメント

| 読むもの | 内容 |
|---|---|
| [j1939-guide.md](j1939-guide.md) | この文書の前に読む、ノード、送受信、部品の使い方 |
| [ecu-executable.md](ecu-executable.md) | ECU を実行ファイルにする方法 |
