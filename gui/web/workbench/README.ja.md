# vehicle-sil Workbench

GUI Client を使って作った英語の GUI サンプルです。標準のブラウザー GUI と同じ GUI サーバーと Control API でシミュレーションを実行します。別のポートで標準画面と同時に使えます。各サーバーはそれぞれ別の実行を持ちます。別タブに開いた Workbench の画面は、開き元の実行を共有します。

## Start

Control Server を有効にしたランチャーと Angular Workbench をビルドします。

```sh
npm --prefix gui/web/workbench ci
npm --prefix gui/web/workbench run build
python3 scripts/run-gui.py --workbench --no-browser --launcher <launcher> <simulation.yaml>
```

コマンドが表示する URL をそのまま開きます。ランチャー、Control Server、Python の準備については [GUI ガイド](../../../docs/ja/gui.md)を参照してください。

## Try a run

- 構成を選び、**Free run** のまま **Start** または **Record & start** を選びます。**Start paused** を選ぶと、最初のステップの前に停止します。
- **ECU I/O** の操作欄を使います。数値を入力して Enter を押すと送信します。スライダーは調整を終えたときに値を送ります。一時停止中の書き込みは ECU のステップで適用されるまで待機します。結果が届かない場合は画面に結果不明と表示されるので、再度書き込む前に値を確認してください。**1 step** はランチャーが示す最小のステップを要求します。
- 信号にフォーカスを置くかポインターを合わせると、固定表示や **Waveforms** への追加ができます。ECU や波形画面は別のブラウザータブにも開けます。別タブの ECU は、選択した参加者を表示し続けます。波形履歴には上限があり、GUI Client で取得し、可能な場合は開いているタブ間で転送します。
- 録画した実行を停止し、**Scenario** の **Load recording** で YAML を確認または保存できます。録画は既存のサーバー形式で、適用された書き込みと仮想時刻を出力し、実行時間をコメントに記録します。再生は保存した YAML の最後のステップで終わります。
- ブラウザーがディレクトリへのアクセスに対応している場合は、**Settings** でローカルフォルダーを選べます。**Scenario** からそのフォルダーの YAML を開いて保存できます。通常のファイル選択とダウンロードも使えます。ファイルを実行するには、選択した構成の隣にあるサーバーの `scenarios/` ディレクトリに置き、Run メニューを更新します。

## Views

**Scenario** は、シナリオ実行後にチェック結果と既存の実行タイムラインを表示します。**Traffic** は、ランチャーが報告するネットワークの概要と外部接続のカウントを表示します。**Logs** は GUI Server のログ、**Results** は実行の判定とステップごとの結果を表示します。**Configuration** では、サーバーの既存の保存ルールに従って YAML を読み込み、チェックし、編集します。

実行前のシナリオタイムラインの図示、CAN フレーム単位の記録、ネットワーク図の編集、任意のローカルシナリオフォルダーからの直接実行には、追加の統合が必要です。
