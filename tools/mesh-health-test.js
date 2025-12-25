#!/usr/bin/env node
/**
 * HID-HOP Mesh Health Test
 * Connects to multiple devices and verifies they can all see each other
 */

const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const DISCOVERY_WAIT_MS = 8000;  // Wait for discovery responses
const DISCOVERY_STAGGER_MS = 500; // Delay between each device's discovery
const RETRY_COUNT = 3;          // Number of discovery attempts

class Device {
    constructor(path) {
        this.path = path;
        this.port = null;
        this.parser = null;
        this.addr = null;
        this.name = null;
        this.peers = [];
        this.appKeyBound = false;
        this.provisioned = false;
        this.pendingResolve = null;
    }

    async connect() {
        return new Promise((resolve, reject) => {
            this.port = new SerialPort({ path: this.path, baudRate: 115200 });
            this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\r\n' }));

            this.parser.on('data', (line) => this.handleData(line));

            this.port.on('error', reject);
            this.port.on('open', () => {
                this.port.set({ dtr: true, rts: true });
                setTimeout(resolve, 500);  // Wait for device to settle
            });
        });
    }

    handleData(line) {
        try {
            const data = JSON.parse(line);

            if (data.event === 'mesh_status') {
                this.addr = data.addr;
                this.name = data.name;
                this.appKeyBound = data.app_key_bound;
                this.provisioned = data.provisioned;
            } else if (data.event === 'peers') {
                this.peers = data.peers || [];
            } else if (data.event === 'info') {
                this.version = data.version;
                this.gitHash = data.git;
                this.buildTime = data.build;
            }

            if (this.pendingResolve) {
                this.pendingResolve(data);
                this.pendingResolve = null;
            }
        } catch (e) {
            // Ignore non-JSON lines
        }
    }

    send(cmd) {
        return new Promise((resolve) => {
            this.pendingResolve = resolve;
            this.port.write(JSON.stringify(cmd) + '\n');
            // Timeout after 2 seconds
            setTimeout(() => {
                if (this.pendingResolve) {
                    this.pendingResolve(null);
                    this.pendingResolve = null;
                }
            }, 2000);
        });
    }

    async getVersion() {
        await this.send({ cmd: 'version' });
        return {
            version: this.version,
            gitHash: this.gitHash,
            buildTime: this.buildTime
        };
    }

    async getStatus() {
        await this.send({ cmd: 'mesh_status' });
        return {
            addr: this.addr,
            name: this.name,
            provisioned: this.provisioned,
            appKeyBound: this.appKeyBound
        };
    }

    async getPeers() {
        await this.send({ cmd: 'peers' });
        return this.peers;
    }

    async discover() {
        await this.send({ cmd: 'mesh_discover' });
    }

    async beacon() {
        await this.send({ cmd: 'beacon' });
    }

    close() {
        if (this.port) {
            this.port.close();
        }
    }
}

async function findPorts() {
    const ports = await SerialPort.list();
    return ports
        .filter(p => p.path.includes('usbmodem'))
        .map(p => p.path);
}

async function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function runTest(portPaths) {
    console.log('\n=== HID-HOP Mesh Health Test ===\n');

    if (portPaths.length < 2) {
        console.error('❌ Need at least 2 devices to test mesh connectivity');
        console.log('   Found:', portPaths.length, 'device(s)');
        process.exit(1);
    }

    const devices = portPaths.map(p => new Device(p));
    const expectedPeerCount = devices.length - 1;

    try {
        // Connect to all devices
        console.log(`Connecting to ${devices.length} devices...`);
        for (const dev of devices) {
            await dev.connect();
            console.log(`  ✓ ${dev.path}`);
        }

        // Get version from all devices
        console.log('\nGetting firmware versions...');
        const versions = new Set();
        for (const dev of devices) {
            const ver = await dev.getVersion();
            const verStr = `${ver.version || '?'}@${ver.gitHash || '?'}`;
            versions.add(verStr);
            console.log(`  ${dev.path}: v${ver.version || '?'} git:${ver.gitHash || '?'} (${ver.buildTime || '?'})`);
        }
        if (versions.size > 1) {
            console.log('  ⚠️  WARNING: Devices running different firmware versions!');
        }

        // Get status from all devices
        console.log('\nGetting device status...');
        for (const dev of devices) {
            const status = await dev.getStatus();
            const icon = status.provisioned && status.appKeyBound ? '✓' : '✗';
            console.log(`  ${icon} ${status.addr} "${status.name}" - prov:${status.provisioned} appkey:${status.appKeyBound}`);

            if (!status.provisioned) {
                console.error(`    ❌ Device not provisioned!`);
            }
            if (!status.appKeyBound) {
                console.error(`    ❌ App key not bound!`);
            }
        }

        // Run discovery multiple times
        let allHealthy = false;
        for (let attempt = 1; attempt <= RETRY_COUNT && !allHealthy; attempt++) {
            console.log(`\nBeacon round ${attempt}/${RETRY_COUNT}...`);

            // Send beacon from all devices (staggered for cleaner logs)
            for (const dev of devices) {
                await dev.beacon();
                console.log(`  → ${dev.addr} sent beacon`);
                await sleep(DISCOVERY_STAGGER_MS);
            }

            // Wait for beacons to propagate
            console.log(`  Waiting ${DISCOVERY_WAIT_MS/1000}s for propagation...`);
            await sleep(DISCOVERY_WAIT_MS);

            // Check peer counts
            console.log('\nPeer check:');
            allHealthy = true;
            for (const dev of devices) {
                const peers = await dev.getPeers();
                const peerAddrs = peers.map(p => p.addr).join(', ');
                const icon = peers.length >= expectedPeerCount ? '✓' : '✗';
                console.log(`  ${icon} ${dev.addr} sees ${peers.length}/${expectedPeerCount} peers: [${peerAddrs}]`);

                if (peers.length < expectedPeerCount) {
                    allHealthy = false;

                    // Which peers are missing?
                    const seenAddrs = new Set(peers.map(p => p.addr));
                    for (const other of devices) {
                        if (other.addr !== dev.addr && !seenAddrs.has(other.addr)) {
                            console.log(`    ⚠ Missing: ${other.addr} "${other.name}"`);
                        }
                    }
                }
            }
        }

        // Final result
        console.log('\n' + '='.repeat(40));
        if (allHealthy) {
            console.log('✅ MESH HEALTHY - All devices see all peers');
        } else {
            console.log('❌ MESH UNHEALTHY - Some peers not discovered');
            console.log('   Try: power cycling devices, checking proximity');
        }
        console.log('='.repeat(40) + '\n');

        return allHealthy;

    } finally {
        // Cleanup
        for (const dev of devices) {
            dev.close();
        }
    }
}

async function main() {
    let portPaths = process.argv.slice(2);

    if (portPaths.length === 0) {
        console.log('Auto-detecting devices...');
        portPaths = await findPorts();
        console.log(`Found: ${portPaths.join(', ') || 'none'}`);
    }

    if (portPaths.length === 0) {
        console.error('No devices found. Specify ports manually:');
        console.log('  node mesh-health-test.js /dev/tty.usbmodem1201 /dev/tty.usbmodem1401');
        process.exit(1);
    }

    const success = await runTest(portPaths);
    process.exit(success ? 0 : 1);
}

main().catch(err => {
    console.error('Error:', err.message);
    process.exit(1);
});
