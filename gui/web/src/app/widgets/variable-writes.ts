import { Injectable, WritableSignal, computed, inject, signal, untracked } from '@angular/core';
import { VariableRef, WriteError } from '@vehicle-sil/gui-client';
import { EcuVariables } from '../core/session/ecu-variables';
import { ServerConnection } from '../core/session/server-connection';
import { SimulationTime } from '../core/session/simulation-time';
import { droppedText } from './labels';
import { WriteBlock, WriteStatus, writeBlock } from './variable-state';

/** 書く値: bool は真偽、数は数、列挙は項目の値（数）か名前。 */
export type WriteValue = number | boolean | string;

/** 送った書き込み 1 つ（どの書き込みかを、オブジェクトそのもので比べる）。 */
interface Sent {
  readonly value: WriteValue;
}

/**
 * 1 つの実行の、1 つの変数への書き込みの様子。書き込み 1 つは、送った（sending）→ 受け取られた（結果待ち）→
 * 確定、と進む。受け取られる前に断られたら、そこで確定する。
 *
 * - `sending`: サーバーがまだ受け取っていない書き込み（多くて 1 つ）。ある間、次の値は送らずに `queued` に覚える。
 *   消すのは、その書き込みの受け付けか確定だけ。
 * - `last`: 最後に送った書き込みで、まだ確定していないもの。`pending`（部品が出す「送った値」）の持ち主。消すのは、
 *   その書き込みの確定だけ（新しく送れば、そちらに替わる）。
 * - `shown`: `status`（様子の欄）が追っている書き込み。新しく送るか覚えると替わり、送る前の誤り（reject）と
 *   「送らなかった」（dropped）は、だれも追わない（null）にして自分の文を出す。追われていない書き込みの受け付けと
 *   確定は、様子の欄を変えない。
 *
 * 送る前の誤りは `shown` と `status` だけを変える。`sending`、`queued`、`last`、`pending` には触れない。
 */
interface Entry {
  session: number;
  ref: VariableRef;
  status: WritableSignal<WriteStatus>;
  pending: WritableSignal<WriteValue | null>;
  sending: Sent | null;
  queued: { value: WriteValue } | null;
  last: Sent | null;
  shown: Sent | null;
}

const keyOf = (ref: VariableRef) => `${ref.peer}\u0000${ref.name}`;

/**
 * 部品からの書き込みを、変数ごとに 1 つにまとめて持つ（アプリ全体で 1 つ）。同じ変数を 2 つの部品に置いても
 * 書き込みは重ならず、絞り込みで部品が作り直されても、送っている途中・失敗の様子は消えない。様子は実行ごとで、
 * 実行が替わると空から始まる（前の実行の「反映しました」や「送った値」を、新しい実行の部品に出さない）。
 *
 * 部品は bindVariable（variable-binding.ts）を通して使う。
 */
@Injectable({ providedIn: 'root' })
export class VariableWrites {
  private readonly variables = inject(EcuVariables);
  private readonly server = inject(ServerConnection);
  private readonly time = inject(SimulationTime);
  private readonly connected = computed(() => this.server.connectionState() === 'open');

  private entries = new Map<string, Entry>();
  private entriesSession: number | null = null;

  /** いま書けない理由（書けるなら null）。部品に依らない分だけ（範囲、値の有無は bindVariable が足す）。 */
  block(ref: VariableRef): WriteBlock | null {
    return writeBlock({
      info: this.variables.find(ref),
      status: this.variables.status(),
      connected: this.connected(),
      beforeStart: this.time.beforeStart(),
      peerConnected: this.variables.peerConnected(ref.peer),
    });
  }

  /** いまの実行での、その変数への最後の書き込みの様子。 */
  status(ref: VariableRef): WriteStatus {
    return this.entry(ref)?.status() ?? IDLE;
  }

  /** 送っていて結果がまだの値（なければ null）。 */
  pending(ref: VariableRef): WriteValue | null {
    return this.entry(ref)?.pending() ?? null;
  }

  /**
   * 書く。書けないときは何もしない。前の書き込みをサーバーがまだ受け取っていなければ、送らずに覚えておき、
   * 受け取ったら最後の値だけを送る（そのとき、実行が同じで、まだ書けるかを確かめ直す）。結果が来なかった
   * 書き込みを、自分で送り直すことはしない。
   */
  write(ref: VariableRef, value: WriteValue): void {
    untracked(() => {
      if (this.block(ref) !== null) return;
      const entry = this.entry(ref);
      if (!entry) return;
      if (entry.sending) {
        // 様子の欄は、送っている書き込みを追う（断られたら、覚えた値も送らないので、その理由を出す）
        entry.queued = { value };
        entry.shown = entry.sending;
        entry.status.set({ kind: 'sending' });
        entry.pending.set(value);
        return;
      }
      this.send(entry, value);
    });
  }

  /**
   * 送る前に部品が見つけた誤り（数でない文字、範囲の外など）を、書き込みの様子として出す。送っている書き込み、
   * 結果を待っている書き込み、覚えている次の値には触れない（すでに送った書き込みの結果は、様子の欄には出ないが、
   * 送った値の表示は確定で消える。覚えている値が後で送られたら、様子の欄はその書き込みを追う）。
   */
  reject(ref: VariableRef, message: string): void {
    untracked(() => {
      const entry = this.entry(ref);
      if (!entry) return;
      entry.shown = null;
      entry.status.set({ kind: 'invalid', message });
    });
  }

  /** いまの実行の、その変数の様子（実行がなければ undefined）。実行が替わったら、前の実行の様子を捨てる。 */
  private entry(ref: VariableRef): Entry | undefined {
    const session = this.variables.session();
    if (session !== this.entriesSession) {
      this.entries = new Map();
      this.entriesSession = session;
    }
    if (session === null) return undefined;
    const key = keyOf(ref);
    let entry = this.entries.get(key);
    if (!entry) {
      entry = {
        session,
        ref: { peer: ref.peer, name: ref.name },
        status: signal<WriteStatus>(IDLE),
        pending: signal<WriteValue | null>(null),
        sending: null,
        queued: null,
        last: null,
        shown: null,
      };
      this.entries.set(key, entry);
    }
    return entry;
  }

  private send(entry: Entry, value: WriteValue): void {
    const mine: Sent = { value };
    const { peer, name } = entry.ref;
    entry.sending = mine;
    entry.last = mine;
    entry.shown = mine;
    entry.status.set({ kind: 'sending' });
    entry.pending.set(value);
    /** サーバーが受け取った: 覚えていた次の値があれば送る。 */
    const accepted = (seq: number | null) => {
      if (entry.sending !== mine) return;
      entry.sending = null;
      const next = entry.queued;
      entry.queued = null;
      if (!next) {
        if (entry.shown === mine && seq !== null) entry.status.set({ kind: 'queued', seq });
        return;
      }
      // ためていた値: 同じ実行で、まだ書けるときだけ送る
      const same = untracked(() => this.variables.session()) === entry.session;
      if (same && untracked(() => this.block(entry.ref)) === null) {
        this.send(entry, next.value);
        return;
      }
      // 送らない。出していたのは送らなかった値なので消す（受け取られた mine の結果は、様子の欄には出さない）
      entry.pending.set(null);
      entry.shown = null;
      entry.status.set({ kind: 'dropped', message: droppedText() });
    };
    /** 確定した（反映、失敗、断られた、答えなし）: もう結果は来ない。 */
    const settled = (status: WriteStatus) => {
      if (entry.sending === mine) {
        // 受け取られる前に終わった: 覚えていた次の値も送らない（同じ理由で断られる）
        entry.sending = null;
        entry.queued = null;
      }
      if (entry.last === mine) {
        entry.last = null;
        entry.pending.set(null);
      }
      if (entry.shown === mine) entry.status.set(status);
    };
    this.variables
      .write(
        { [peer]: { [name]: value } },
        {
          // 止まっている間は、進めるまで結果が来ない。時間で切らずに、操作の結果を待つ（実行の終わり、実行の
          // 替わり、つなぎ直しで結果が失われたときは、ライブラリが「答えが来なかった」で確定させる）
          timeoutMs: Infinity,
          onAccepted: ({ seq }) => accepted(seq),
        },
      )
      .then(
        (operation) => {
          accepted(null); // ふつうは onAccepted が先に済ませている
          settled({ kind: 'applied', atMs: operation.peers[peer]?.appliedAtMs ?? null });
        },
        (error: unknown) =>
          settled(
            error instanceof WriteError
              ? { kind: error.kind, message: error.message, code: error.code }
              : { kind: 'no-answer', message: String(error), code: null },
          ),
      );
  }
}

const IDLE: WriteStatus = { kind: 'idle' };
