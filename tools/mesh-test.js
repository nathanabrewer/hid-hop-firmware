#!/usr/bin/env node
/**
 * HID-HOP Mesh Test Tool
 * Connects to two dongles and tests mesh communication
 */

const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

class Dongle {
    constructor(name, path) {
        this.name = name;
        this.path = path;
        this.port = null;
        this.parser = null;
        this.addr = null;
        this.events = [];
        this.waiters = [];
    }

    async connect() {
        return new Promise((resolve, reject) => {
            this.port = new SerialPort({ path: this.path, baudRate: 115200 });
            this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\r\n' }));

            this.parser.on('data', (line) => {
                try {
                    const data = JSON.parse(line);
                    this.events.push(data);
                    console.log(`  [${this.name}] ← ${JSON.stringify(data)}`);

                    // Check waiters
                    this.waiters = this.waiters.filter(w => {
                        if (w.check(data)) {
                            w.resolve(data);
                            return false;
                        }
                        return true;
                    });
                } catch {
                    console.log(`  [${this.name}] ← (raw) ${line}`);
                }
            });

            this.port.on('error', reject);
            this.port.on('open', () => {
                console.log(`  [${this.name}] Connected to ${this.path}`);
                setTimeout(resolve, 500); // Wait for device
            });
        });
    }

    send(cmd) {
        const json = JSON.stringify(cmd);
        console.log(`  [${this.name}] → ${json}`);
        this.port.write(json + '\n');
    }

    waitFor(check, timeout = 5000) {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.waiters = this.waiters.filter(w => w !== waiter);
                reject(new Error('Timeout waiting for event'));
            }, timeout);

            const waiter = {
                check,
                resolve: (data) => {
                    clearTimeout(timer);
                    resolve(data);
                }
            };
            this.waiters.push(waiter);
        });
    }

    async getStatus() {
        this.send({ cmd: 'mesh_status' });
        const status = await this.waitFor(d => d.event === 'mesh_status');
        this.addr = status.addr;
        return status;
    }

    async waitForAppKey(timeout = 15000) {
        const start = Date.now();
        while (Date.now() - start < timeout) {
            const status = await this.getStatus();
            if (status.app_key_bound) return status;
            console.log(`\n    Waiting for app key binding...`);
            await sleep(1000);
        }
        throw new Error('App key binding timeout');
    }

    close() {
        if (this.port) this.port.close();
    }
}

async function findPorts() {
    const ports = await SerialPort.list();
    return ports.filter(p => p.path.includes('usbmodem')).map(p => p.path);
}

async function sleep(ms) {
    return new Promise(r => setTimeout(r, ms));
}

async function test(name, fn) {
    process.stdout.write(`\n${name}... `);
    try {
        await fn();
        console.log('\x1b[32mPASS\x1b[0m');
        return true;
    } catch (err) {
        console.log(`\x1b[31mFAIL: ${err.message}\x1b[0m`);
        return false;
    }
}

async function main() {
    console.log('\n=== HID-HOP Mesh Test ===\n');

    // Find ports
    const ports = await findPorts();
    console.log(`Found ${ports.length} USB modem(s):`);
    ports.forEach(p => console.log(`  ${p}`));

    if (ports.length < 2) {
        console.log('\nNeed 2 dongles connected for mesh test.');
        console.log('Usage: node mesh-test.js [port1] [port2]');

        if (ports.length === 1) {
            console.log('\nRunning single-dongle tests...\n');
            const d1 = new Dongle('D1', ports[0]);
            await d1.connect();

            await test('Ping', async () => {
                d1.send({ cmd: 'ping' });
                await d1.waitFor(d => d.event === 'pong');
            });

            await test('Mesh Status (wait for app key)', async () => {
                const status = await d1.waitForAppKey();
                if (!status.provisioned) throw new Error('Not provisioned');
                console.log(`\n    addr=${status.addr}, founder=${status.founder}`);
            });

            await test('Self Discovery', async () => {
                d1.send({ cmd: 'mesh_discover' });
                await d1.waitFor(d => d.event === 'discovery_rcvd');
            });

            d1.close();
        }
        return;
    }

    // Connect to both dongles
    const port1 = process.argv[2] || ports[0];
    const port2 = process.argv[3] || ports[1];

    const d1 = new Dongle('D1', port1);
    const d2 = new Dongle('D2', port2);

    try {
        console.log('\nConnecting...');
        await d1.connect();
        await d2.connect();

        // Get status (wait for app key binding)
        await test('D1 Status (wait for app key)', async () => {
            const s = await d1.waitForAppKey();
            if (!s.provisioned) throw new Error('Not provisioned');
            console.log(`\n    addr=${s.addr}`);
        });

        await test('D2 Status (wait for app key)', async () => {
            const s = await d2.waitForAppKey();
            if (!s.provisioned) throw new Error('Not provisioned');
            console.log(`\n    addr=${s.addr}`);
        });

        // Test mesh communication
        await test('D1 → D2 Discovery', async () => {
            d1.events = [];
            d2.events = [];

            d1.send({ cmd: 'mesh_discover' });

            // D2 should receive discovery from D1
            const rcvd = await d2.waitFor(d =>
                d.event === 'discovery_rcvd' && d.from === d1.addr,
                10000
            );
            console.log(`\n    D2 received discovery from ${rcvd.from}`);
        });

        await test('D2 → D1 Discovery', async () => {
            d1.events = [];
            d2.events = [];

            d2.send({ cmd: 'mesh_discover' });

            // D1 should receive discovery from D2
            const rcvd = await d1.waitFor(d =>
                d.event === 'discovery_rcvd' && d.from === d2.addr,
                10000
            );
            console.log(`\n    D1 received discovery from ${rcvd.from}`);
        });

        await test('D1 → D2 Ping', async () => {
            d2.events = [];

            d1.send({ cmd: 'mesh_ping', addr: d2.addr });

            const rcvd = await d2.waitFor(d =>
                d.event === 'mesh_ping_rcvd' && d.from === d1.addr,
                10000
            );
            console.log(`\n    D2 received ping from ${rcvd.from}`);
        });

        await test('D2 → D1 Ping', async () => {
            d1.events = [];

            d2.send({ cmd: 'mesh_ping', addr: d1.addr });

            const rcvd = await d1.waitFor(d =>
                d.event === 'mesh_ping_rcvd' && d.from === d2.addr,
                10000
            );
            console.log(`\n    D1 received ping from ${rcvd.from}`);
        });

        console.log('\n\x1b[32m=== All tests passed! ===\x1b[0m\n');

    } catch (err) {
        console.error('\nTest error:', err.message);
    } finally {
        d1.close();
        d2.close();
    }
}

main().catch(console.error);
