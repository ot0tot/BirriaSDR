# SPDX-License-Identifier: MIT
"""Exercise the actual standalone UI against a running demo or bench server."""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build-tools/standalone-browser'))
from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', default='http://127.0.0.1:8073')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--hardware', action='store_true')
    parser.add_argument('--browser-channel', default='msedge')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    report = dict(url=args.url, hardware=args.hardware, errors=[], checks=[])
    def save():
        (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', newline='\n')
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(channel=args.browser_channel, headless=True)
        page = browser.new_page(viewport=dict(width=1536, height=1080), device_scale_factor=1)
        page.on('pageerror', lambda error: report['errors'].append(str(error)))
        try:
            page.goto(args.url)
            page.wait_for_function("() => document.querySelector('#device').textContent.indexOf('Discovering') < 0")
            page.get_by_role('button', name='Start receiver').click()
            page.wait_for_function("() => document.querySelector('#empty').hidden && Number(document.querySelector('#totalSamples').textContent.replace(/[^0-9.]/g,'')) > 0", timeout=30000)
            page.wait_for_timeout(3000)
            report['initial'] = page.evaluate("fetch('/api/state').then(r=>r.json()).then(({token,...s})=>s)")
            assert report['initial']['mode'] == ('hardware' if args.hardware else 'demo')
            if args.hardware:
                assert report['initial']['display_handoff'] == 'native'
            page.screenshot(path=str(args.out / 'spectrum-40.png'))
            page.get_by_role('button', name='＋', exact=True).click()
            page.wait_for_timeout(100)
            page.get_by_role('button', name='Full span').click()
            page.get_by_label('Peak hold').check()
            page.locator('#manual').check()
            page.locator('#lna').fill('3')
            page.get_by_role('button', name='Apply gain', exact=True).click()
            page.wait_for_function("() => document.querySelector('#run').disabled === false")
            page.locator('#rate').select_option('80000000')
            page.locator('#bandwidth').select_option('80000000')
            page.get_by_role('button', name='Apply receiver settings').click()
            page.wait_for_function("() => document.querySelector('#run').disabled === false")
            page.wait_for_function("() => document.querySelector('#plotInfo').textContent.startsWith('80 MSps')")
            page.wait_for_timeout(1000)
            page.get_by_role('button', name='5.8 GHz', exact=True).click()
            page.wait_for_function("() => document.querySelector('#run').disabled === false")
            page.wait_for_function("() => document.querySelector('#frequency').value === '5800.000000'")
            page.wait_for_timeout(25000 if args.hardware else 2000)
            report['wide'] = page.evaluate("fetch('/api/state').then(r=>r.json()).then(({token,...s})=>s)")
            assert report['wide']['active'] and not report['wide']['error'], report['wide']
            page.screenshot(path=str(args.out / 'spectrum-80-high.png'))
            with page.expect_download() as download:
                page.get_by_role('button', name='Save I/Q snapshot').click()
            download.value.save_as(str(args.out / 'snapshot.zip'))
            report['checks'].append('40/80 MSps, high-band tune, gain, zoom, peak hold and raw snapshot')
            page.get_by_role('button', name='Stop receiver').click()
            page.wait_for_function("() => document.querySelector('#run').textContent.includes('Start receiver')", timeout=30000)
            report['stopped'] = page.evaluate("fetch('/api/state').then(r=>r.json()).then(({token,...s})=>s)")
            assert not report['stopped']['active'] and not report['stopped']['error'], report['stopped']
            with page.expect_download() as diagnostics:
                page.get_by_role('button', name='Save diagnostics', exact=True).click()
            diagnostics.value.save_as(str(args.out / 'diagnostics.json'))
            assert 'token' not in json.loads((args.out / 'diagnostics.json').read_text())
            if args.hardware:
                assert report['stopped']['final_stats']['samples'] > 80000000 * 20
                assert report['stopped']['final_stats']['dma_errors'] == 0
            page.set_viewport_size(dict(width=700, height=1000))
            # A hidden canvas has a zero layout width during responsive changes.
            # Pointer events must not create non-finite drawing coordinates.
            page.evaluate("""() => {
                const canvas = document.querySelector('#spectrum');
                canvas.style.display = 'none';
                canvas.dispatchEvent(new Event('pointerleave'));
                canvas.style.display = '';
            }""")
            page.wait_for_timeout(250)
            report['checks'].append('narrow viewport and hidden-canvas drawing guard')
            page.screenshot(path=str(args.out / 'narrow-layout.png'), full_page=True)
            assert not report['errors'], report['errors']
            report['passed'] = True
        except Exception as error:
            report['error'] = repr(error)
            page.screenshot(path=str(args.out / 'failure.png'), full_page=True)
            raise
        finally:
            # Keep the server available, release the receiver even on UI failure.
            try:
                page.evaluate("fetch('/api/state').then(r=>r.json()).then(s=>fetch('/api/stop',{method:'POST',headers:{'X-Birria-Token':s.token}}))")
            finally:
                save()
                browser.close()
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
