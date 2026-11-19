import {
  Component,
  DOCUMENT,
  computed,
  effect,
  inject,
  linkedSignal,
  resource,
  signal,
  viewChild,
  ElementRef,
} from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCardModule } from '@angular/material/card';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatSelectChange, MatSelectModule } from '@angular/material/select';
import { ActionError, CheckResult, Finding, errorMessage } from '@vehicle-sil/gui-client';
import { AuthToken } from '../core/auth/auth-token';
import { GUI_FRONT } from '../core/session/gui-client';
import { RunCommands } from '../core/session/run-commands';
import { ServerConnection } from '../core/session/server-connection';
import { ServerInfo } from '../core/session/server-info';
import { checkOutcomeLabel, checkStatusLabel } from '../widgets/check-labels';
import { controlLine } from '../widgets/control-labels';
import { FindingsList } from '../run/findings-list';
import {
  ChannelView,
  DesignerModel,
  addNetwork,
  open,
  participantNames,
  removeNetwork,
  text,
  view,
  wire,
} from './designer-model';

/** 文を直してから照合を頼むまでの間（ミリ秒）。続けて直したら、最後の文で 1 回だけ照合する。 */
export const DESIGN_CHECK_DELAY_MS = 300;

/** 足せるネットワークの種類（表に出す選択）。 */
const KINDS = ['can', 'lin', 'ethernet'] as const;

/** 中断できる待ち。 */
function wait(ms: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    signal.throwIfAborted();
    const timer = setTimeout(() => {
      signal.removeEventListener('abort', abort);
      resolve();
    }, ms);
    const abort = () => {
      clearTimeout(timer);
      reject(signal.reason);
    };
    signal.addEventListener('abort', abort, { once: true });
  });
}

/** 照合の答えと、その照合に送った文（表は、答えの行番号が合う文から作る）。 */
interface Answer {
  sent: string;
  result: CheckResult;
}

/**
 * Network Designer: 構成ファイルの `networks` と、各 participant の実行ファイルのネットワークの名前をシミュレーションの
 * ネットワークにつなぐ `network_names` を、表で書き換える。画面は規則を持たない: 表は文の YAML と、その文の照合
 * （`check` の `text`）の答えだけから作り、誤りは次の照合の所見が言う。保存は後の版で、ここではダウンロードする。
 * 表で書き換えるのは共通の部分だけ。それ以外（`command`、`variants`、`external` など）は文の欄で書く。
 */
@Component({
  selector: 'app-design-page',
  imports: [
    MatButtonModule,
    MatCardModule,
    MatFormFieldModule,
    MatInputModule,
    MatSelectModule,
    FindingsList,
  ],
  templateUrl: './design-page.html',
  styleUrl: './design-page.css',
})
export class DesignPage {
  protected readonly auth = inject(AuthToken);
  private readonly info = inject(ServerInfo);
  private readonly front = inject(GUI_FRONT);
  private readonly commands = inject(RunCommands);
  /** この画面を開いている間、WebSocket をつなぐ（保存の可否 `controls.save` は、サーバーの知らせで決まる）。 */
  private readonly connection = inject(ServerConnection);
  private readonly document = inject(DOCUMENT);

  protected readonly kinds = KINDS;

  protected readonly configs = resource({
    params: () => this.auth.token() ?? undefined,
    loader: () => this.info.configs(),
  });

  private readonly chosen = signal<string | null>(null);
  /** 選んでいる構成ファイル: 選んだもの、起動のときに渡されたもの、一覧の最初、の順。 */
  protected readonly file = computed(() => {
    const listed = this.configs.hasValue() ? this.configs.value().files.map((c) => c.path) : [];
    return this.chosen() ?? this.info.initialFile() ?? listed[0] ?? null;
  });

  private readonly reloads = signal(0);
  /** 保存してあるファイルの文。 */
  protected readonly loaded = resource({
    params: () => {
      const file = this.file();
      return this.auth.token() && file !== null ? { file, reloads: this.reloads() } : undefined;
    },
    loader: ({ params }) => this.front.readConfig({ file: params.file }),
  });
  protected readonly loadedText = computed(() =>
    this.loaded.hasValue() ? this.loaded.value().text : null,
  );

  /** 読んだときのファイルの stamp（保存のとき、ファイルが外で変わっていないことの確かめに送る）。保存したら、返る stamp にする。 */
  protected readonly stamp = linkedSignal<string | null, string | null>({
    source: () => (this.loaded.hasValue() ? this.loaded.value().stamp : null),
    computation: (source) => source,
  });

  /** 読んだ（または保存した）ファイルの文。変更の印は、これと今の文の差。保存したら、送った文になる。 */
  protected readonly readText = linkedSignal<string | null, string | null>({
    source: () => this.loadedText(),
    computation: (source) => source,
  });

  /** いま編集している文。ファイルを読んだら、その文になる。 */
  protected readonly draft = linkedSignal<string | null, string | null>({
    source: () => this.loadedText(),
    computation: (source) => source,
  });
  protected readonly changed = computed(
    () => this.draft() !== null && this.draft() !== this.readText(),
  );

  /** 照合する variant（なければ null）。ファイルを替えたら選び直す。 */
  protected readonly variant = linkedSignal<string | null, string | null>({
    source: () => this.file(),
    computation: () => null,
  });

  private readonly again = signal(0);
  /** 文の照合。文を直したら、少し待ってから 1 回（古い答えは捨てる）。 */
  protected readonly check = resource({
    params: () => {
      const file = this.file();
      const draft = this.draft();
      if (file === null || draft === null || !this.auth.token()) return undefined;
      return { file, text: draft, variant: this.variant(), again: this.again() };
    },
    loader: async ({ params, abortSignal }): Promise<Answer> => {
      await wait(DESIGN_CHECK_DELAY_MS, abortSignal);
      const result = await this.front.check(
        params.variant === null ? {} : { variant: params.variant },
        { file: params.file, text: params.text, signal: abortSignal },
      );
      abortSignal.throwIfAborted();
      return { sent: params.text, result };
    },
  });
  /** 最後に読めた答え（照合し直している間も残す）。ファイルを替えたら消す。 */
  protected readonly answer = linkedSignal<
    { value: Answer | undefined; file: string | null },
    Answer | null
  >({
    source: () => ({
      value: this.check.hasValue() ? this.check.value() : undefined,
      file: this.file(),
    }),
    computation: (now, previous) =>
      now.value ?? (previous && previous.source.file === now.file ? previous.value : null),
  });
  protected readonly checkError = computed(() => {
    const error = this.check.error();
    return error ? errorMessage(error) : null;
  });
  /** 答えが、いまの文のものか（直した直後は、まだ前の文の答え）。 */
  protected readonly upToDate = computed(
    () => this.answer()?.sent === this.draft() && !this.check.isLoading(),
  );
  /** 照合の答えを待っている（照合が失敗した後は、待っていない）。 */
  protected readonly checking = computed(() => !this.upToDate() && !this.checkError());

  /** 保存できるか（サーバーの `controls.save`）と、できない理由、注意。 */
  protected readonly saveControl = computed(() => this.commands.controls().save);
  protected readonly saving = signal(false);
  protected readonly saveProblem = signal<{ message: string; findings: string[] } | null>(null);
  private readonly justSaved = signal(false);
  /** 「保存しました」: 保存した文のまま（次の編集で消える）。 */
  protected readonly savedNote = computed(() => this.justSaved() && !this.changed());
  /** 保存できない理由と注意の 1 行（実行の画面の操作の理由と同じ作り方。持ち主の英語の文のときは `english`）。 */
  protected readonly saveLine = computed(() => controlLine('save', this.saveControl()));

  /** 保存してあるファイルの共通の部分の participant。「ECU を読み直す」で申告を読み直せるのは、これだけ。 */
  private readonly savedNames = computed(() => {
    const saved = this.readText();
    return saved === null ? new Set<string>() : participantNames(open(saved));
  });
  /** 「ECU を読み直す」の後も申告を読めなかった participant（保存してから、でないと照合できない）。 */
  private readonly stuck = linkedSignal<string | null, ReadonlySet<string>>({
    source: () => this.file(),
    computation: () => new Set<string>(),
  });
  /** 申告を読んでいない participant のうち、保存した構成にいて、読み直せるもの。 */
  protected readonly rereadable = computed(() =>
    (this.design()?.notDescribed ?? []).filter(
      (name) => this.savedNames().has(name) && !this.stuck().has(name),
    ),
  );
  /** 申告を読んでいない participant のうち、読み直しても読めないもの（文で足した、`command` を変えた）。 */
  protected readonly unsaved = computed(() =>
    (this.design()?.notDescribed ?? []).filter(
      (name) => !this.savedNames().has(name) || this.stuck().has(name),
    ),
  );

  /** 表: 答えを送った文と、その答えから作る。 */
  protected readonly table = computed(() => {
    const a = this.answer();
    if (!a) return null;
    return { model: open(a.sent), result: a.result };
  });
  protected readonly design = computed(() => {
    const t = this.table();
    return t ? view(t.model, t.result) : null;
  });
  /** いま編集している文を開いたもの（読めなければ表は止まる）。 */
  protected readonly model = computed<DesignerModel | null>(() => {
    const draft = this.draft();
    return draft === null ? null : open(draft);
  });
  protected readonly editable = computed(() => this.model()?.editable ?? false);
  protected readonly parseErrors = computed(() => this.model()?.errors ?? []);
  protected readonly json = computed(() => this.model()?.json ?? false);

  protected readonly findings = computed<readonly Finding[]>(
    () => this.answer()?.result.findings ?? [],
  );
  protected readonly lines = computed(() =>
    [
      ...new Set(
        this.findings()
          .map((f) => f.line)
          .filter((n): n is number => n !== null),
      ),
    ].sort((a, b) => a - b),
  );
  protected readonly outcome = computed(() => {
    const result = this.answer()?.result;
    return result ? checkOutcomeLabel(result.result) : null;
  });

  /** 編集の失敗の文（英語）。 */
  protected readonly editProblem = signal<string | null>(null);
  protected readonly describing = signal(false);
  protected readonly describeProblem = signal<string | null>(null);

  protected readonly newName = signal('');
  protected readonly newKind = signal<string>('ethernet');

  private readonly editor = viewChild<ElementRef<HTMLTextAreaElement>>('editor');

  constructor() {
    // ファイルを選び直したら、前のファイルの編集の失敗は消す
    effect(() => {
      this.file();
      this.editProblem.set(null);
      this.describeProblem.set(null);
      this.justSaved.set(false);
    });
  }

  /** 直した文を捨てるなら、利用者に確かめる（保存はダウンロードだけなので、捨てると戻せない）。 */
  private discardOk(): boolean {
    if (!this.changed()) return true;
    const ask = this.document.defaultView?.confirm;
    return ask
      ? ask.call(
          this.document.defaultView,
          $localize`直した文は、ダウンロードしていなければ消えます。捨てますか。`,
        )
      : true;
  }

  protected selectFile(change: MatSelectChange): void {
    if (change.value === this.file()) return;
    if (!this.discardOk()) {
      change.source.value = this.file();
      return;
    }
    this.chosen.set(change.value);
  }

  protected selectVariant(name: string | null): void {
    this.variant.set(name);
  }

  protected statusLabel(status: string): string {
    return checkStatusLabel(status);
  }

  /** 文を、表の操作で書き換える。書き換えられなければ（共通の部分にいない participant など）、理由を出す。 */
  private edit(change: (model: DesignerModel) => void): boolean {
    const draft = this.draft();
    if (draft === null) return false;
    const model = open(draft);
    if (!model.editable) return false;
    try {
      change(model);
    } catch (error) {
      this.editProblem.set(errorMessage(error));
      return false;
    }
    this.editProblem.set(null);
    this.draft.set(text(model));
    return true;
  }

  protected add(): void {
    const name = this.newName().trim();
    if (name === '') return;
    this.edit((m) => addNetwork(m, name, this.newKind()));
    this.newName.set('');
  }

  protected remove(name: string): void {
    this.edit((m) => removeNetwork(m, name));
  }

  /** 選択を表の操作にする。書けなかったら、選択を元に戻す（表は文から作るので、選択だけが先へ進まないように）。 */
  protected connect(participant: string, channel: ChannelView, change: MatSelectChange): void {
    if (!this.edit((m) => wire(m, participant, channel.from, change.value))) {
      change.source.value = channel.network;
    }
  }

  /** つなぐ先の選択肢: すべてのネットワーク（種類で絞らない）。実行ファイルの名前のままと、宣言のない現在の先も出す。 */
  protected options(channel: ChannelView): { value: string; asIs: boolean }[] {
    const names = (this.design()?.networks ?? []).map((n) => n.name);
    const list = names.map((value) => ({ value, asIs: value === channel.from }));
    if (!names.includes(channel.from)) list.push({ value: channel.from, asIs: true });
    if (!names.includes(channel.network) && channel.network !== channel.from) {
      list.push({ value: channel.network, asIs: false });
    }
    return list;
  }

  /** 行の所見の code（participant か network で結ぶ）。 */
  protected codesOf(by: { participant?: string; network?: string }): string[] {
    return this.findings()
      .filter((f) =>
        by.participant !== undefined ? f.participant === by.participant : f.network === by.network,
      )
      .map((f) => f.code);
  }

  /**
   * いまの文を、読んだファイルへ保存する（`saveConfig`）。保存できたら、stamp を覚え、ファイルを読み直して照合する
   * （保存した participant は「ECU を読み直す」で読めるようになる）。失敗は理由の文で出す。
   */
  protected async save(): Promise<void> {
    const file = this.file();
    const draft = this.draft();
    if (file === null || draft === null || this.saving() || !this.saveControl().ok) return;
    this.saving.set(true);
    this.saveProblem.set(null);
    this.justSaved.set(false);
    try {
      const saved = await this.front.saveConfig({ file, text: draft, stamp: this.stamp() });
      // 送った文を「読んだ文」にする（返事を待つ間に直した分は残る）。読み直さないので、画面は消えない
      this.stamp.set(saved.stamp);
      this.readText.set(draft);
      this.justSaved.set(true);
      this.stuck.set(new Set());
      this.again.update((n) => n + 1);
    } catch (error) {
      if (!(error instanceof ActionError)) throw error;
      if (
        error.code === 'config-changed' &&
        this.document.defaultView?.confirm(
          $localize`ファイルが、この画面で読んだ後に外で変わりました。読み直しますか（直した文は消えます）。`,
        )
      ) {
        this.reloads.update((n) => n + 1);
        return;
      }
      this.saveProblem.set({
        message: error.message,
        findings: error.findings.map((f) => f.message),
      });
    } finally {
      this.saving.set(false);
    }
  }

  /** 入力欄の、いまの文字。 */
  protected valueOf(event: Event): string {
    return (event.target as HTMLInputElement | HTMLTextAreaElement).value;
  }

  protected typed(value: string): void {
    // 文の欄は改行を LF で返す。読んだファイルが CRLF なら、CRLF に戻す
    this.draft.set(this.readText()?.includes('\r\n') ? value.replace(/\r?\n/g, '\r\n') : value);
  }

  protected revert(): void {
    this.draft.set(this.readText());
  }

  protected reload(): void {
    if (!this.discardOk()) return;
    this.justSaved.set(false);
    this.reloads.update((n) => n + 1);
  }

  /** 文の欄の、その行を選ぶ。 */
  protected goToLine(line: number): void {
    const area = this.editor()?.nativeElement;
    const draft = this.draft();
    if (!area || draft === null) return;
    const rows = draft.split(/\r?\n/);
    const start = rows.slice(0, line - 1).reduce((n, r) => n + r.length + 1, 0);
    area.focus();
    area.setSelectionRange(start, start + (rows[line - 1]?.length ?? 0));
  }

  /** 保存してあるファイルの ECU を読み直す（実行ファイルを作り直した後）。答えは次の照合が使う。 */
  protected async describeAgain(): Promise<void> {
    const file = this.file();
    if (file === null || this.describing()) return;
    this.describing.set(true);
    this.describeProblem.set(null);
    const pressed = this.design()?.notDescribed ?? [];
    try {
      await this.front.describeAgain({
        file,
        ...(this.variant() !== null ? { variant: this.variant() as string } : {}),
      });
      // 押した時点で申告を読んでいなかった participant は、読み直しでは直らない側にする（文で足した、`command` を変えた
      // ものは、保存した構成と違う）。読み直しで直ったものは、次の照合で「読んでいない」から抜ける
      this.stuck.set(new Set(pressed));
      this.again.update((n) => n + 1);
    } catch (error) {
      this.describeProblem.set(errorMessage(error));
    } finally {
      this.describing.set(false);
    }
  }

  /** いまの文を、ファイルとして保存させる（ブラウザのダウンロード。置き場所は利用者が決める）。 */
  protected download(): void {
    const draft = this.draft();
    const file = this.file();
    if (draft === null || file === null) return;
    const url = URL.createObjectURL(new Blob([draft], { type: 'text/yaml;charset=utf-8' }));
    const link = this.document.createElement('a');
    link.href = url;
    link.download = file.split('/').pop() ?? 'simulation.yaml';
    link.click();
    setTimeout(() => URL.revokeObjectURL(url));
  }
}
