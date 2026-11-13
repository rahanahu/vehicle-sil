# 変更の記録

vehicle-sil の版ごとの変更です。版の番号は `MAJOR.MINOR.PATCH` で、0.x の間は MINOR を上げる版で互換を壊すことがあります。互換を壊す変更には、利用者がすることを書きます。

版の番号の出どころは、ルートの `CMakeLists.txt` の `project(vehicle_sil VERSION ...)` です（`vehicle_sil_launch version`、ECU の `--describe` の `vehicle_sil` がこれを出します）。版を上げるときは、同じ番号を `pyproject.toml`、`gui/server/pyproject.toml`、`template/get_tools.py` の `VERSION`、`template/CMakeLists.txt` の `GIT_TAG`、このファイルの見出しにも書きます。`scripts/package.py version` が、食い違いを見つけます。

## 0.1.0（未リリース）

最初の版です。

- ECU を書くためのライブラリ（`vehicle_sil::Ecu`、`ecu_main()`、J1939 のスタックと部品、型付きメッセージ、LIN、ECU の変数）と、SIL Kit の上で ECU の実行ファイルをまとめて動かすランチャー（`vehicle_sil_launch check`、`run`、シナリオ、Control API）、ブラウザの GUI。
- ビルド済みの道具（OS ごとに 1 つの圧縮ファイル: `vehicle_sil_launch`、GUI サーバーと画面、Control API の Python クライアント）と、ECU のプロジェクトのひな形（`template/`。リリースでは `vehicle-sil-template-0.1.0.zip`）。
- CMake の選択肢 `VEHICLE_SIL_STATIC`: ECU の実行ファイルに SIL Kit と C++ の実行時ライブラリを静的にリンクし、1 つのファイルで配れるようにする（既定は OFF）。

互換を壊す変更: なし（最初の版）。
