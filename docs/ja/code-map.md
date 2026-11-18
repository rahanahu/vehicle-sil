# コードの地図

[English](../code-map.md)

<!-- 書き方: 項目は、同じ形の短いブロックにします（小見出し、1 行に 1 つの事実の箇条書き、セルが 1 文の表）。1 つのセルや 1 つの箇条書きに複数の事実を詰めません。1 行が長くなり、別々の変更が同じ行に当たるためです。 -->

vehicle-sil 自体を開発するときに、変える場所、そのテスト、テストだけを実行する方法、一緒に直すものを探すための地図です。各ディレクトリと各ターゲットの内容は [architecture.md](architecture.md) の「ディレクトリとターゲット」にあり、ここには書きません。

## 作業対象から探す

まず変更対象のページを読みます。そこで示す仕様と、必要なときだけ隣接する分野の地図を読みます。テストの共通規則はテストのページにあります。実際の登録名とリンク先は CMake と CTest を正とします。図や変更時の注意は設計上の制約を示すために残します。

| 作業対象 | 地図 |
|---|---|
| テストの開発 | [code-map-testing.md](code-map-testing.md) |
| 基盤・ステップ・信号・SIL Kit の既定 | [code-map-core.md](code-map-core.md) |
| CAN・イーサネット・LIN のバス | [code-map-buses.md](code-map-buses.md) |
| J1939 エンジン・コンポーネント・コーデック・spy | [code-map-j1939.md](code-map-j1939.md) |
| ECU・Peer・ローカル実行・Simulation | [code-map-execution.md](code-map-execution.md) |
| ランチャー・プロセス・設定・実行 | [code-map-launcher.md](code-map-launcher.md) |
| シナリオ・時間の軸・一括実行 | [code-map-scenario.md](code-map-scenario.md) |
| Control API・gRPC・ランチャーの Control Server | [code-map-control.md](code-map-control.md) |
| Vector XL、SocketCAN、イーサネットのブリッジ | [code-map-bridges.md](code-map-bridges.md) |
| ブラウザー GUI・プロトコル・クライアント・画面の例 | [code-map-gui.md](code-map-gui.md) |
| ビルド・配布・依存ライブラリ・ライブラリの追加 | [code-map-build.md](code-map-build.md) |

## 観点から探す

- テストの命名、登録、実行、待ち時間: [共通のテスト規則](code-map-testing.md#テストの名前と実行方法)
- テストのラベルや `ctest -R`: 対象領域の「テストのラベル」、「ディレクトリごとのコードとテスト」を参照してください。共通ラベルは [テストのラベル](code-map-testing.md#テストのラベル)、LIN はバス、仮想時間は実行のページを参照してください。GUI は GUI のページにある検証スクリプトを使います。
- 依存関係: [全体の層と図の凡例](code-map-core.md#依存の向き)。詳細の図は [J1939](code-map-j1939.md#依存の向き)、[ECU・実行](code-map-execution.md#依存の向き)、[Control API・ランチャー](code-map-control.md#依存の向き)、[ブリッジ](code-map-bridges.md#依存の向き)、[ビルド・その他の実行ファイル](code-map-build.md#依存の向き)。
- [境界を検査するテスト](code-map-testing.md#境界を検査するテスト)
- [文書の例とテスト](code-map-testing.md#ドキュメントの例とテスト)
- 変更に合わせて直すもの: 各領域の「変更するときに一緒に直すもの」を参照してください。領域をまたぐ変更は該当節からのリンクをたどります。

## 領域ページの書式

どの項目も、主なクラスと関数の表（ヘッダーは `include/` の下のパス）、テストのディレクトリ、`ctest -R` に渡す正規表現の順に書いてあります。

どの項目も、変える場所、合わせて直すドキュメント、合わせて直すコード、守ること、テストの順に書いてあります（ない見出しは省いています）。「手で合わせる」と書いたものは、テストが照合しません。

領域ページに項目を追加するときは、関連する変更時の注意や参照先をその領域に置きます。この索引に個々の見出しを追加したり、すべてのリンクに逆向きのリンクを作ったりする必要はありません。

## 関連するドキュメント

| ドキュメント | 内容 |
|---|---|
| [AGENTS.md](../../AGENTS.md) | ドキュメントの管理、コミット、検証のスクリプト（先に読みます） |
| [architecture.md](architecture.md) | 全体の構成と、各ディレクトリと各ターゲットの内容 |
| [internals/](architecture.md#内部の仕組み) のドキュメント | 変える場所の内部の仕組み |
