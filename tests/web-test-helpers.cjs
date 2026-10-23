const fs = require('node:fs');

// ImGui trickles down/up and other interleaved input across separate frames.
// Two browser animation callbacks can finish while clicks are still queued,
// especially with software rendering at high DPI. Wait for actual app frames
// and the input queue, without imposing a fixed delay on fast machines.
async function settle(page) {
  const frame = await page.evaluate(() => coldeditWeb.diagnostics().frame_count);
  await page.waitForFunction(frame => {
    const d = coldeditWeb.diagnostics();
    return d.frame_count >= frame + 2 && d.pending_input_events === 0;
  }, frame);
}

async function captureFailure(page, name, error) {
  const state = await page.locator('#canvas').evaluate(() => ({
    diagnostics: window.coldeditWeb?.diagnostics(),
    notice: document.querySelector('#notice-text')?.textContent,
    downloadReady: !document.querySelector('#download-export')?.hidden,
  }), null, {timeout: 5000}).catch(e => ({diagnosticsError: String(e)}));
  fs.writeFileSync(`artifacts/${name}.json`, JSON.stringify({error: String(error), ...state}, null, 2));
  await page.screenshot({path: `artifacts/${name}.png`, timeout: 5000}).catch(() => {});
}

module.exports = {settle, captureFailure};
