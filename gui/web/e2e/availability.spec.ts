// ECU が変数をいま使っているかの見え方（表示だけの情報）を、流し直すランチャーと偽の Control Server で通す。
// 替え玉の ECU は、engine.demand と engine.speed の利用状態を知らせる（REPLAY_AVAILABILITY=1）: 最初は unknown、
// 最初のステップのあと demand は inactive、speed は active。
import { expect, test } from '@playwright/test';
import { CONFIGS, REPLAY_LAUNCHER, GuiServer, launchGuiServer } from './support/gui-server';
import {
  Target,
  openVariables,
  startRun,
  stopRun,
  timeCommand,
  widget,
  widgetValue,
} from './support/variables-page';

const CONTROL_FEATURES =
  'events,stdin-commands,request,stdin-run-until,state,check-variables,speed,start-paused,time-commands,control-links,control-server';

let server: GuiServer;
let target: Target;

test.beforeAll(async () => {
  test.skip(test.info().project.name !== 'static', 'only with the built screen');
  server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    env: {
      REPLAY_TICK_MS: '50',
      REPLAY_FEATURES: CONTROL_FEATURES,
      REPLAY_VARIABLES: 'more',
      REPLAY_AVAILABILITY: '1',
    },
    port: 0,
  });
  target = { base: new URL('/', server.url).href, token: server.token };
});

test.afterAll(async () => {
  await server?.stop();
});

test.afterEach(async ({ page }) => {
  if (target) await stopRun(page, target);
});

test('a variable the ECU does not use now is dimmed, and is still written and read', async ({
  page,
}) => {
  await startRun(page, target, 'forever.yaml', true);
  await openVariables(page, target);
  const demand = widget(page, 'Engine', 'engine.demand');
  const speed = widget(page, 'Engine', 'engine.speed');
  const slider = demand.getByRole('slider', { name: 'engine.demand' });

  // 最初のステップの前: unknown と、知らせのない変数（active）は、ふつうに出す
  await expect(demand).toHaveAttribute('data-availability', 'unknown');
  await expect(speed).toHaveAttribute('data-availability', 'unknown');
  await expect(widget(page, 'Engine', 'engine.enabled')).toHaveAttribute(
    'data-availability',
    'active',
  );
  await expect(demand.locator('.unused')).toHaveCount(0);

  // 最初のステップのあと: demand は inactive（値と操作が薄く、見出しに印と title）、speed は active
  await timeCommand(page, target, 'resume');
  await expect(demand).toHaveAttribute('data-availability', 'inactive');
  await expect(speed).toHaveAttribute('data-availability', 'active');
  await expect(demand.locator('.unused')).toHaveAttribute('title', 'この ECU はいま使っていません');
  await expect(demand.locator('.body')).toHaveCSS('opacity', '0.6');
  await expect(demand.locator('.unused')).toBeVisible();
  await expect(demand).toHaveCSS('opacity', '1');

  // 操作は止まらない: 書け（反映される）、無効の印は付かない
  await expect(slider).toBeEnabled();
  await expect(demand.locator('[aria-disabled="true"]')).toHaveCount(0);
  await slider.focus();
  await page.keyboard.press('End');
  await expect(demand.getByTestId('widget-write')).toHaveText(/反映しました/);
  // inactive の変数の値も届き続ける（書いた値が戻ってくる）
  await expect(widgetValue(page, 'Engine', 'engine.demand')).toHaveText('100.00 %');
  await expect(demand).toHaveAttribute('data-availability', 'inactive');
});
