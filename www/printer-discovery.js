/**
 * PrinterDiscovery - Auto-discover Raspberry Pi Print Server on the local network.
 *
 * Solves the dynamic-IP problem for PWAs that need to reach a local print server.
 *
 * How it works:
 *   1. Checks localStorage for a previously found server URL
 *   2. Pings it to verify it's still alive
 *   3. If not found, scans common local subnets for the server
 *   4. Caches the result for instant reconnection next time
 *
 * Usage in your PWA:
 *
 *   <script src="printer-discovery.js"></script>
 *   <script>
 *     const printer = new PrinterClient();
 *
 *     // Auto-discover and connect
 *     const connected = await printer.connect();
 *     if (!connected) {
 *       // Show manual IP entry UI
 *       await printer.connect({ url: 'https://192.168.1.171:5124' });
 *     }
 *
 *     // Print
 *     await printer.printText('POS-80', 'Hello World!');
 *
 *     // Get printers list
 *     const printers = await printer.getPrinters();
 *   </script>
 */

class PrinterDiscovery {
  constructor(options = {}) {
    this.httpPort = options.httpPort || 5123;
    this.httpsPort = options.httpsPort || 5124;
    this.timeout = options.timeout || 3000;
    this.storageKey = options.storageKey || 'rpi_server_url';
    this.preferHttps = options.preferHttps !== false;
    this.onStatus = options.onStatus || (() => {});
  }

  async findServer() {
    const cached = this._getCached();
    if (cached) {
      this.onStatus('Checking saved server...');
      const info = await this._ping(cached);
      if (info) return { url: cached, ...info };
      this._clearCache();
      this.onStatus('Saved server unreachable, scanning network...');
    } else {
      this.onStatus('Scanning local network for print server...');
    }

    const found = await this.scan();
    if (found.length > 0) {
      const server = found[0];
      this._setCache(server.url);
      this.onStatus(`Found server at ${server.url}`);
      return server;
    }

    this.onStatus('Print server not found on the network.');
    return null;
  }

  async scan(subnets) {
    if (!subnets) {
      subnets = this._guessSubnets();
    }

    const results = [];
    for (const subnet of subnets) {
      this.onStatus(`Scanning ${subnet}.x ...`);
      const found = await this._scanSubnet(subnet);
      results.push(...found);
      if (results.length > 0) break;
    }
    return results;
  }

  async _scanSubnet(subnet) {
    const results = [];
    const batchSize = 25;

    for (let start = 1; start <= 254; start += batchSize) {
      const end = Math.min(start + batchSize - 1, 254);
      const promises = [];

      for (let i = start; i <= end; i++) {
        promises.push(this._tryHost(`${subnet}.${i}`));
      }

      const batch = await Promise.allSettled(promises);
      for (const r of batch) {
        if (r.status === 'fulfilled' && r.value) results.push(r.value);
      }
      if (results.length > 0) break;
    }

    return results;
  }

  async _tryHost(ip) {
    const attempts = this.preferHttps
      ? [
          { proto: 'https', port: this.httpsPort },
          { proto: 'http', port: this.httpPort },
        ]
      : [
          { proto: 'http', port: this.httpPort },
          { proto: 'https', port: this.httpsPort },
        ];

    for (const { proto, port } of attempts) {
      const url = `${proto}://${ip}:${port}`;
      const info = await this._ping(url);
      if (info) return { url, ip, ...info };
    }
    return null;
  }

  async _ping(baseUrl) {
    try {
      const controller = new AbortController();
      const tid = setTimeout(() => controller.abort(), this.timeout);
      const res = await fetch(`${baseUrl}/api/discover`, {
        signal: controller.signal,
        mode: 'cors',
        cache: 'no-cache',
      });
      clearTimeout(tid);
      if (!res.ok) return null;
      const data = await res.json();
      return data.service === 'RaspberryPiPrintServer' ? data : null;
    } catch {
      return null;
    }
  }

  _guessSubnets() {
    return [
      '192.168.1',
      '192.168.0',
      '192.168.2',
      '192.168.10',
      '192.168.100',
      '10.0.0',
      '10.0.1',
      '172.16.0',
    ];
  }

  _getCached() {
    try {
      return localStorage.getItem(this.storageKey);
    } catch {
      return null;
    }
  }
  _setCache(url) {
    try {
      localStorage.setItem(this.storageKey, url);
    } catch {
      /* storage unavailable */
    }
  }
  _clearCache() {
    try {
      localStorage.removeItem(this.storageKey);
    } catch {
      /* storage unavailable */
    }
  }

  setManualUrl(url) {
    this._setCache(url);
  }
  clearSavedUrl() {
    this._clearCache();
  }
}

/**
 * PrinterClient - High-level API for printing from a PWA.
 *
 * Wraps PrinterDiscovery with a simple print/command interface.
 */
class PrinterClient {
  constructor(options = {}) {
    this._discovery = new PrinterDiscovery(options);
    this._serverUrl = null;
    this._serverInfo = null;
  }

  get serverUrl() {
    return this._serverUrl;
  }
  get serverInfo() {
    return this._serverInfo;
  }
  get isConnected() {
    return this._serverUrl !== null;
  }

  async connect(options = {}) {
    if (options.url) {
      const info = await this._discovery._ping(options.url);
      if (info) {
        this._serverUrl = options.url;
        this._serverInfo = info;
        this._discovery.setManualUrl(options.url);
        return true;
      }
      return false;
    }

    const server = await this._discovery.findServer();
    if (server) {
      this._serverUrl = server.url;
      this._serverInfo = server;
      return true;
    }
    return false;
  }

  disconnect() {
    this._serverUrl = null;
    this._serverInfo = null;
    this._discovery.clearSavedUrl();
  }

  async _ensureConnected() {
    if (!this._serverUrl) {
      const ok = await this.connect();
      if (!ok) throw new Error('Print server not found. Call connect() first or configure manually.');
    }
  }

  async _api(method, endpoint, body) {
    await this._ensureConnected();
    const opts = { method, mode: 'cors', cache: 'no-cache' };
    if (body !== undefined) {
      opts.headers = { 'Content-Type': 'application/json' };
      opts.body = JSON.stringify(body);
    }
    const res = await fetch(`${this._serverUrl}/api${endpoint}`, opts);
    return res.json();
  }

  async getPrinters() {
    return this._api('GET', '/printers');
  }

  async getPrinterStatus(name) {
    return this._api('GET', `/printers/status/${encodeURIComponent(name)}`);
  }

  async printText(printer, text, options = {}) {
    return this._api('POST', '/print/text', { printer, text, ...options });
  }

  async printRaw(printer, base64Data) {
    return this._api('POST', '/print/raw', { printer, base64Data });
  }

  async printTest(printer, options = {}) {
    return this._api('POST', '/print/test', { printer, ...options });
  }

  async beep(printer, count = 2, duration = 3) {
    return this._api('POST', '/print/beep', { printer, count, duration });
  }

  async cut(printer, cutType = 'full') {
    return this._api('POST', '/print/cut', { printer, cutType, feedLines: 3 });
  }

  async openDrawer(printer) {
    return this._api('POST', '/print/open-drawer', { printer });
  }

  async printImage(printer, imageFile, options = {}) {
    await this._ensureConnected();
    const fd = new FormData();
    fd.append('image', imageFile);
    fd.append('printer', printer);
    fd.append('paperWidth', String(options.paperWidth || 48));
    fd.append('cutPaper', String(options.cutPaper !== false));
    fd.append('feedLines', String(options.feedLines || 3));
    if (options.brightness) fd.append('brightness', String(options.brightness));
    if (options.gamma) fd.append('gamma', String(options.gamma));
    if (options.dithering) fd.append('dithering', options.dithering);
    if (options.threshold) fd.append('threshold', String(options.threshold));

    const res = await fetch(`${this._serverUrl}/api/print/image`, {
      method: 'POST',
      body: fd,
      mode: 'cors',
    });
    return res.json();
  }
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { PrinterDiscovery, PrinterClient };
}
