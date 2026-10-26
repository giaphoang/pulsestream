// Optional Playwright smoke test against the isolated fixture in ui_smoke.py.
const { chromium } = require(process.env.PULSE_PLAYWRIGHT_MODULE || 'playwright');
const assert = require('node:assert/strict');
const path = require('node:path');
(async () => {
  const browser = await chromium.launch({headless: true, executablePath: process.env.PULSE_CHROMIUM_PATH || undefined});
  try {
    const page = await browser.newPage({viewport: {width: 1280, height: 900}});
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(process.env.PULSE_UI_URL);
    await page.getByLabel('Clinic bearer credential').fill(process.env.PULSE_UI_CLINIC);
    await page.getByLabel('Synthetic device ID').fill(process.env.PULSE_UI_DEVICE);
    await page.getByRole('button', {name: 'Device status', exact: true}).click();
    await page.getByRole('cell', {name: 'connected', exact: true}).waitFor();
    await page.getByRole('button', {name: 'Notifications', exact: true}).click();
    await page.getByRole('cell', {name: 'synthetic_review_flag', exact: true}).waitFor();
    await page.getByRole('button', {name: 'Acknowledge', exact: true}).click();
    await page.getByRole('button', {name: 'Acknowledged', exact: true}).waitFor();
    assert(await page.getByRole('button', {name: 'Acknowledged', exact: true}).isDisabled());
    await page.getByLabel('Operator credential', {exact: true}).fill(process.env.PULSE_UI_OPERATOR);
    await page.getByRole('button', {name: 'Refresh queue metrics', exact: true}).click();
    await page.waitForFunction(() => document.getElementById('metrics').textContent.includes('pulse_queue_pending 0'));
    await page.screenshot({path: path.resolve('docs/evidence/dashboard.png'), fullPage: true});
    await page.getByLabel('Clinic bearer credential').fill(process.env.PULSE_UI_OTHER_CLINIC);
    await page.getByRole('button', {name: 'Notifications', exact: true}).click();
    await page.getByText('No records available.', {exact: true}).waitFor();
    await page.setViewportSize({width: 390, height: 844});
    await page.getByRole('button', {name: 'Audit records', exact: true}).click();
    await page.getByText('No records available.', {exact: true}).waitFor();
    assert.deepEqual(errors, []);
    console.log('Dashboard: status, delivery, acknowledgment, metrics, clinic isolation and mobile view passed');
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
