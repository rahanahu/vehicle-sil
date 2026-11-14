# 変更の記録

vehicle-sil の版ごとの変更です。版の番号は `MAJOR.MINOR.PATCH` で、0.x の間は MINOR を上げる版で互換を壊すことがあります。互換を壊す変更には、利用者がすることを書きます。

版の番号の出どころは、ルートの `CMakeLists.txt` の `project(vehicle_sil VERSION ...)` です（`vehicle_sil_launch version`、ECU の `--describe` の `vehicle_sil` がこれを出します）。版を上げるときは、同じ番号を `pyproject.toml`、`gui/server/pyproject.toml`、`template/get_tools.py` の `VERSION`、`template/CMakeLists.txt` の `GIT_TAG`、`template/README.md` に書いた版、このファイルの見出しにも書きます。`scripts/package.py version` が、食い違いを見つけます。

## 0.1.0（未リリース）

最初の版です。

- ECU を書くためのライブラリ（`vehicle_sil::Ecu`、`ecu_main()`、J1939 のスタックと部品、型付きメッセージ、LIN、ECU の変数）と、SIL Kit の上で ECU の実行ファイルをまとめて動かすランチャー（`vehicle_sil_launch check`、`run`、シナリオ、Control API）、ブラウザの GUI。
- ビルド済みの道具（OS ごとに 1 つの圧縮ファイル: `vehicle_sil_launch`、GUI サーバーと画面、Control API の Python クライアント、GUI のクライアントのライブラリ、自分の画面のひな形 `gui_starter/`、ダッシュボードの JSON Schema）と、ECU のプロジェクトのひな形（`template/`。リリースでは `vehicle-sil-template-0.1.0.zip`）。
- CMake の選択肢 `VEHICLE_SIL_STATIC`: ECU の実行ファイルに SIL Kit と C++ の実行時ライブラリを静的にリンクし、1 つのファイルで配れるようにする（既定は OFF）。
- 待ち受けるアドレス: `registry_uri` がループバックのアドレスなら、registry と vehicle-sil が作る participant はループバックのアドレスだけで待ち受けます。ほかのアドレスなら、複数の PC にまたがる構成として全部のアドレスで待ち受け、`check` と `run` が注記 `registry-on-network` を出します。`run` は起動する participant に `VEHICLE_SIL_LISTEN_ON_NETWORK` を渡します（[docs/simulation-run.md](docs/simulation-run.md) の「待ち受けるアドレス」）。
- ECU の自己申告（`--describe`）の形式の版は、キーの意味が変わるか、キーがなくなるときだけ上がります。値が足されただけなら、版の違う ECU とランチャーを混ぜて使えます。ランチャーの知らない値は、その ECU だけの誤り `describe-unknown-value` になります。
- シナリオを流している実行にも、Control API でつなげます（`run --scenario` と `--control`）。クライアントは読む、待つ、止めるだけで、書く呼び出しと時刻を動かす呼び出しは断られます。
- ブラウザの GUI に自分の画面を配れます（`vehicle-sil-gui --web <dir>`）。クライアントのライブラリは、型つきの npm のパッケージ `vehicle-sil-gui-client-<版>.tgz` でも配ります。`client.variables.bind` で、どの枠組みからでも変数 1 つの値、新しさ、書けるか、書き込みの状態を 1 つの値で受け取れます。
- Vector XL CAN Bridge: バスの異常を 1 秒に 1 行まで知らせます（`--no-bus-monitor` で止める）。デバイスを外すと終了コード 1 で終わります。送信レシートでバスに出たフレームを数え（`own_tx_dropped`）、止めるときに送れなかった数を `xl_tx_unsent` に出します。`--tx-timeout-ms` を付けると、バスが詰まっている間は新しいフレームを捨てます（`xl_tx_dropped`）。バスオフが続くとチャネルを有効にし直します（`--bus-off-restart-ms`、既定 1000、0 で無効）。この 2 つは `--bitrate`（init 権限）があるときだけ効きます。
- ECU が終わるとき、取り消された書き込みなどを運ぶ最後の知らせが、ランチャーに届くまで待ちます（ランチャーが返事をし、ECU は長くても 500 ms 待ちます。ふつうはすぐ終わります）。実行の終わりに ECU が取り消した書き込みは、`Unknown` ではなく `Cancelled` で確定します。ランチャーが先に終わっていたときだけ、ECU の終わりが 500 ms 遅れます。
- 生のイーサネットのフレームの土台のライブラリ: `vehicle_sil::eth`（ステップの中で送受信するチャネル、プロセス内の仮想ネットワーク。SIL Kit なし）と `vehicle_sil::eth_silkit`（SIL Kit の Ethernet のポート、仮想時間）。同じ時刻に届いたフレームは送り元のアドレスの順に渡るので、仮想ネットワークと SIL Kit で同じ記録になります（[docs/internals/ethernet.md](docs/internals/ethernet.md)）。
- イーサネットのブリッジのライブラリ: SIL Kit を使わない外のインターフェースの口と絞り込み（`vehicle_sil::eth_link`。EtherType、VLAN タグの先、SIL Kit の側の MAC、最大長）と、SIL Kit の Ethernet との間でフレームを運ぶ `EthernetBridge`（`vehicle_sil::ethernet_bridge_core`）。同じネットワークの間の 2 つ目のブリッジや両側にある MAC は、送り元 MAC から見つけられる場合に見つけ、そのアドレスのフレームを両方向で捨てて警告します（見つけられない場合は文書に書いてあります）。統計の 1 行、監視の行、引数の解釈を含みます（[docs/ethernet-bridge.md](docs/ethernet-bridge.md)）。
- 構成（`cmake --preset …`）のときに、ビルドする SIL Kit のコミット、当てたパッチ、写しの場所を 1 行で出します。
- `scripts/doctor.py`: この PC に、vehicle-sil のビルドと試験に要るものがそろっているかを確かめます（読むだけです。足りないものには入れ方を出します。`--json`）。
- 開発中にあった単独の Control Server（`vehicle_sil_control_server`）は、この版に入れていません。Control API には `vehicle_sil_launch run --control` でつなぎ、ランチャーに起動させない ECU は participant の `manual`（または `run --manual <name>`）で同じ実行に加えます。

互換を壊す変更: なし（最初の版）。
