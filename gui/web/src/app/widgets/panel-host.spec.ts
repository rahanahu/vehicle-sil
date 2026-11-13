import { provideHttpClient } from '@angular/common/http';
import { provideHttpClientTesting } from '@angular/common/http/testing';
import { Injectable } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { FakeWebSockets } from '@vehicle-sil/gui-client/testing';
import { GUI_CLIENT, SOCKET_FACTORY } from '../core/session/gui-client';
import { DashboardPanel } from './dashboard-spec';
import { PanelHost, PanelModules } from './panel-host';

// 持ち込みのパネルの枠: モジュールを読み終えてから要素を作り、client、options、variables を渡してページに入れる。
// 読み込めない、定義されていない、作れないときは、その場所に理由を出す。

/** 読み込みの代わり: URL ごとに、要素を定義するか失敗する。 */
@Injectable()
class FakeModules extends PanelModules {
  readonly imported: string[] = [];

  protected override importModule(href: string): Promise<unknown> {
    this.imported.push(href);
    if (href.endsWith('/broken.js')) {
      return Promise.reject(new SyntaxError(`Unexpected token at ${href}`));
    }
    if (href.endsWith('/good.js') && !customElements.get('test-good-panel')) {
      customElements.define(
        'test-good-panel',
        class extends HTMLElement {
          connectedCallback() {
            const self = this as unknown as { options: { max: number }; variables: unknown[] };
            this.textContent = `max ${self.options.max}, ${self.variables.length} variable`;
          }
        },
      );
    }
    return Promise.resolve({});
  }
}

describe('PanelHost', () => {
  let modules: FakeModules;

  beforeEach(() => {
    TestBed.configureTestingModule({
      providers: [
        provideHttpClient(),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: new FakeWebSockets().factory },
        { provide: PanelModules, useClass: FakeModules },
      ],
    });
    modules = TestBed.inject(PanelModules) as FakeModules;
  });

  /** モジュールの名前から、配る URL の形（key 入り）を作って渡す。 */
  async function render(panel: DashboardPanel, names: string[]) {
    const modules = names.map((module) => ({ module, url: `panels/KEY/${module}` }));
    const fixture = TestBed.createComponent(PanelHost);
    fixture.componentRef.setInput('panel', panel);
    fixture.componentRef.setInput('modules', modules);
    fixture.detectChanges();
    await fixture.whenStable();
    await new Promise((resolve) => setTimeout(resolve, 0)); // the modules' promises
    fixture.detectChanges();
    return fixture.nativeElement as HTMLElement;
  }

  it('makes the element with the client, the options and the variables, once its module is loaded', async () => {
    const host = await render(
      {
        panel: 'test-good-panel',
        label: 'Speed',
        options: { max: 4000 },
        variables: [{ peer: 'Engine', name: 'engine.speed' }],
      },
      ['panels/good.js'],
    );
    const element = host.querySelector('test-good-panel') as HTMLElement & Record<string, unknown>;
    expect(element.textContent).toBe('max 4000, 1 variable');
    expect(element['client']).toBe(TestBed.inject(GUI_CLIENT));
    expect(element['variables']).toEqual([{ peer: 'Engine', name: 'engine.speed' }]);
    expect(host.querySelector('header')?.textContent).toBe('Speed');
    expect(host.querySelector('[data-testid="panel-problem"]')).toBeNull();
    expect(modules.imported).toEqual([new URL('panels/KEY/panels/good.js', document.baseURI).href]);
  });

  it('says why when the module does not load or does not define the element', async () => {
    let host = await render({ panel: 'test-missing-panel' }, ['panels/broken.js']);
    let problem = host.querySelector('[data-testid="panel-problem"]')?.textContent ?? '';
    expect(problem).toContain('test-missing-panel');
    // the name in the file, not the URL with the server's key (the browser's own text may hold the URL)
    expect(problem).toContain('panels/broken.js: Unexpected token at panels/broken.js');
    expect(problem).not.toContain('KEY');
    expect(host.querySelector('test-missing-panel')).toBeNull();

    host = await render({ panel: 'test-missing-panel' }, ['panels/good.js']);
    problem = host.querySelector('[data-testid="panel-problem"]')?.textContent ?? '';
    expect(problem).toContain('customElements.define');
  });

  it('draws an element defined before, and says that a module of this file did not load', async () => {
    await render({ panel: 'test-good-panel', options: { max: 1 } }, ['panels/good.js']);
    // another configuration's panel of the same name: its module throws (already defined)
    const host = await render({ panel: 'test-good-panel', options: { max: 1 } }, [
      'panels/broken.js',
    ]);
    expect(host.querySelector('test-good-panel')).not.toBeNull();
    const problem = host.querySelector('[data-testid="panel-problem"]')?.textContent ?? '';
    expect(problem).toContain('前に読み込んだ定義');
    expect(problem).toContain('panels/broken.js');
  });

  it('loads each module once', async () => {
    await render({ panel: 'test-good-panel' }, ['panels/good.js']);
    await render({ panel: 'test-good-panel' }, ['panels/good.js']);
    expect(modules.imported).toHaveLength(1);
  });
});
