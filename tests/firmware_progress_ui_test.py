"""Run the actual embedded download renderer with a minimal DOM (no hardware)."""
from pathlib import Path
import shutil
import subprocess
import unittest


class FirmwareProgressUI(unittest.TestCase):
    @unittest.skipUnless(shutil.which('node'), 'Node.js unavailable for browser renderer test')
    def test_download_renderer(self):
        page = (Path(__file__).parents[1] / 'main/diagnostics.cpp').read_text()
        start = page.index('function renderDownloadProgress(')
        render = page[start:page.index('async function refreshFirmware()', start)]
        script = r'''
const assert=require('assert');
const nodes={};
const el=id=>nodes[id]||(nodes[id]={hidden:false,textContent:'',removeAttribute(k){delete this[k];}});
'''+render+r'''
renderDownloadProgress({});
assert(el('fw-download').hidden);
const f={update_state:'downloading',download_progress:{started:true,received_bytes:1024,total_bytes:2048}};
renderDownloadProgress(f);
assert(!el('fw-download').hidden);assert.equal(el('fw-progress').value,50);
assert(el('fw-progress-text').textContent.includes('50%'));
f.download_progress.total_bytes=0;renderDownloadProgress(f);
assert(!('value' in el('fw-progress')));
assert(el('fw-progress-text').textContent.includes('total size unknown'));
f.download_progress.total_bytes=2048;f.download_progress.received_bytes=2048;
f.update_state='verifying';renderDownloadProgress(f);
assert.equal(el('fw-progress').value,100);
assert(el('fw-progress-text').textContent.includes('Verifying'));
f.update_state='failed';renderDownloadProgress(f);
assert(el('fw-progress-text').textContent.includes('failed'));
f.update_state='rebooting';renderDownloadProgress(f);
assert(el('fw-progress-text').textContent.includes('Verified; rebooting'));
f.download_progress.started=false;renderDownloadProgress(f);
assert(el('fw-download').hidden);
'''
        subprocess.run(['node', '-e', script], check=True, capture_output=True, text=True)
