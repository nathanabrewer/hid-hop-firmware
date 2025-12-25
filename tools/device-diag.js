#!/usr/bin/env node
/**
 * HID-HOP Device Diagnostics
 * Comprehensive mesh diagnostic - tests all devices simultaneously
 */

const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const TIMEOUT_MS = 3000;
const BEACON_TEST_MS = 10000;  // Time to wait for beacon propagation

class DeviceDiag {
    constructor(path) {
        this.path = path;
        this.port = null;
        this.parser = null;
        this.events = [];
        this.lastResponse = null;
        this.resolver = null;
        this.addr = null;
        this.name = null;
        this.version = null;
        this.provisioned = false;
        this.appKeyBound = false;
        this.beaconsReceived = new Map();  // addr -> {count, rssi[]}
    }

    async connect() {
        return new Promise((resolve, reject) => {
            this.port = new SerialPort({ path: this.path, baudRate: 115200 });
            this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\r\n' }));

            this.parser.on('data', (line) => {
                try {
                    const data = JSON.parse(line);
                    this.events.push({ time: Date.now(), data });
                    this.lastResponse = data;

                    // Track beacon receives
                    if (data.event === 'beacon_rcvd') {
                        const from = data.from;
                        if (!this.beaconsReceived.has(from)) {
                            this.beaconsReceived.set(from, { count: 0, rssi: [] });
                        }
                        const entry = this.beaconsReceived.get(from);
                        entry.count++;
                        entry.rssi.push(data.rssi);
                    }

                    // Call resolver if waiting - resolver decides if it matched
                    if (this.resolver) {
                        this.resolver(data);
                        // Note: resolver clears itself when matched
                    }
                } catch (e) {
                    // Ignore non-JSON
                }
            });

            this.port.on('error', reject);
            this.port.on('open', () => {
                this.port.set({ dtr: true, rts: true });
                setTimeout(resolve, 500);
            });
        });
    }

    async send(cmd, expectedEvent = null) {
        return new Promise((resolve) => {
            const timeout = setTimeout(() => {
                this.resolver = null;
                resolve(null);
            }, TIMEOUT_MS);

            this.resolver = (data) => {
                // If we're looking for a specific event, filter
                if (expectedEvent && data.event !== expectedEvent) {
                    return; // Keep waiting
                }
                clearTimeout(timeout);
                this.resolver = null;
                resolve(data);
            };

            this.port.write(JSON.stringify(cmd) + '\n');
        });
    }

    clearBeaconStats() {
        this.beaconsReceived.clear();
    }

    close() {
        if (this.port) this.port.close();
    }

    get shortPath() {
        return this.path.replace('/dev/tty.usbmodem', '');
    }
}

async function sleep(ms) {
    return new Promise(r => setTimeout(r, ms));
}

async function runMultiDeviceDiag(portPaths) {
    console.log('\n' + '='.repeat(60));
    console.log('HID-HOP MESH DIAGNOSTICS - ALL DEVICES');
    console.log('='.repeat(60));
    console.log(`\nTesting ${portPaths.length} devices simultaneously\n`);

    const devices = portPaths.map(p => new DeviceDiag(p));

    try {
        // Phase 1: Connect all
        console.log('PHASE 1: CONNECTING');
        console.log('-'.repeat(40));
        for (const dev of devices) {
            try {
                await dev.connect();
                console.log(`  ✓ ${dev.shortPath}`);
            } catch (err) {
                console.log(`  ✗ ${dev.shortPath} - ${err.message}`);
            }
        }
        console.log('');

        // Phase 2: Get info from all
        console.log('PHASE 2: DEVICE INFO');
        console.log('-'.repeat(40));
        const infoTable = [];
        for (const dev of devices) {
            if (!dev.port) continue;

            const version = await dev.send({ cmd: 'version' }, 'info');
            const mesh = await dev.send({ cmd: 'mesh_status' }, 'mesh_status');

            if (version?.event === 'info') {
                dev.version = version.version;
                dev.gitHash = version.git;
            }
            if (mesh?.event === 'mesh_status') {
                dev.addr = mesh.addr;
                dev.name = mesh.name;
                dev.provisioned = mesh.provisioned;
                dev.appKeyBound = mesh.app_key_bound;
            }

            const status = dev.provisioned && dev.appKeyBound ? '✓' : '✗';
            infoTable.push({
                port: dev.shortPath,
                addr: dev.addr || '?',
                name: dev.name || '?',
                version: dev.version || '?',
                git: dev.gitHash || '?',
                prov: dev.provisioned ? 'Y' : 'N',
                appkey: dev.appKeyBound ? 'Y' : 'N',
                status
            });
        }

        // Print table
        console.log('  Port      Addr     Name         Version  Git      Prov  AppKey');
        console.log('  ' + '-'.repeat(65));
        for (const row of infoTable) {
            console.log(`  ${row.status} ${row.port.padEnd(8)} ${row.addr.padEnd(8)} ${row.name.padEnd(12).slice(0,12)} ${row.version.padEnd(8)} ${row.git.padEnd(8)} ${row.prov.padEnd(5)} ${row.appkey}`);
        }
        console.log('');

        // Check for version/git mismatches
        const gitHashes = new Set(devices.filter(d => d.gitHash).map(d => d.gitHash));
        if (gitHashes.size > 1) {
            console.log('  ⚠ WARNING: Different firmware builds detected!');
            console.log('    Expected git hash: 793387f (or latest)');
            console.log('    Reflash devices with mismatched firmware.');
            console.log('');
        } else if (gitHashes.size === 1 && !gitHashes.has('793387f')) {
            console.log(`  ⚠ WARNING: Firmware may be outdated (git: ${[...gitHashes][0]})`);
            console.log('');
        }

        // Phase 3: Beacon TX test - each device sends, track who receives
        console.log('PHASE 3: BEACON PROPAGATION TEST');
        console.log('-'.repeat(40));
        console.log(`  Each device sends beacon, others listen for ${BEACON_TEST_MS/1000}s total\n`);

        // Clear all beacon stats
        for (const dev of devices) {
            dev.clearBeaconStats();
        }

        // Have each device send a beacon with small delay between
        for (const dev of devices) {
            if (!dev.port || !dev.addr) continue;

            // Clear events before sending to see what comes in
            const beforeCount = dev.events.length;

            const result = await dev.send({ cmd: 'beacon' }, 'beacon');

            // Show what events came in during the wait
            const newEvents = dev.events.slice(beforeCount);
            if (newEvents.length > 0 && result === null) {
                const eventTypes = newEvents.map(e => e.data.event || 'unknown').join(', ');
                console.log(`  ✗ ${dev.addr} (${dev.shortPath}) TIMEOUT - got events: [${eventTypes}]`);
            } else if (result?.event === 'beacon' && result.sent) {
                console.log(`  → ${dev.addr} (${dev.shortPath}) sent beacon`);
            } else if (result?.event === 'beacon' && result.error) {
                console.log(`  ✗ ${dev.addr} (${dev.shortPath}) beacon error: ${result.error}`);
            } else if (result === null) {
                console.log(`  ✗ ${dev.addr} (${dev.shortPath}) beacon TIMEOUT (no events received)`);
            } else {
                console.log(`  ✗ ${dev.addr} (${dev.shortPath}) unexpected: ${JSON.stringify(result)}`);
            }
            await sleep(500);  // Stagger beacons
        }

        // Wait for propagation
        console.log(`\n  Waiting ${BEACON_TEST_MS/1000}s for propagation...`);
        await sleep(BEACON_TEST_MS);

        // Phase 4: Connectivity Matrix
        console.log('\nPHASE 4: CONNECTIVITY MATRIX');
        console.log('-'.repeat(40));
        console.log('  Who received beacons from whom:\n');

        // Build matrix header
        const addrs = devices.filter(d => d.addr).map(d => d.addr);
        const header = '  RECEIVER   │ ' + addrs.map(a => a.slice(-4).padEnd(6)).join(' ');
        console.log(header);
        console.log('  ' + '─'.repeat(12) + '┼' + '─'.repeat(addrs.length * 7));

        // Build matrix rows
        const rxIssues = [];
        const txIssues = [];

        for (const receiver of devices) {
            if (!receiver.addr) continue;

            let row = `  ${receiver.addr.padEnd(10)} │ `;
            let receivedCount = 0;

            for (const sender of devices) {
                if (!sender.addr) {
                    row += '  ?   ';
                    continue;
                }

                if (sender.addr === receiver.addr) {
                    row += '  -   ';
                    continue;
                }

                const stats = receiver.beaconsReceived.get(sender.addr);
                if (stats && stats.count > 0) {
                    const avgRssi = Math.round(stats.rssi.reduce((a,b) => a+b, 0) / stats.rssi.length);
                    row += `${stats.count}(${avgRssi})`.padEnd(6) + ' ';
                    receivedCount++;
                } else {
                    row += '  ✗   ';
                }
            }

            console.log(row);

            // Track issues
            const expectedRx = addrs.length - 1;  // All except self
            if (receivedCount === 0 && expectedRx > 0) {
                rxIssues.push(receiver);
            } else if (receivedCount < expectedRx) {
                // Partial RX - which senders weren't heard?
            }
        }

        // Check TX issues (who wasn't received by anyone)
        for (const sender of devices) {
            if (!sender.addr) continue;
            let receivedByAny = false;
            for (const receiver of devices) {
                if (receiver.addr !== sender.addr && receiver.beaconsReceived.has(sender.addr)) {
                    receivedByAny = true;
                    break;
                }
            }
            if (!receivedByAny && devices.length > 1) {
                txIssues.push(sender);
            }
        }

        console.log('');
        console.log('  Legend: count(avg_rssi) or ✗=not received, -=self');
        console.log('');

        // Phase 5: Diagnosis
        console.log('PHASE 5: DIAGNOSIS');
        console.log('-'.repeat(40));

        let healthy = true;

        // Check for unpaired/unbound devices
        const configIssues = devices.filter(d => !d.provisioned || !d.appKeyBound);
        if (configIssues.length > 0) {
            healthy = false;
            console.log('\n  ✗ MESH CONFIG ISSUES:');
            for (const dev of configIssues) {
                if (!dev.provisioned) {
                    console.log(`    - ${dev.addr || dev.shortPath}: Not provisioned`);
                } else if (!dev.appKeyBound) {
                    console.log(`    - ${dev.addr}: App key not bound`);
                }
            }
        }

        // RX issues
        if (rxIssues.length > 0) {
            healthy = false;
            console.log('\n  ✗ RX ISSUES (cannot receive beacons):');
            for (const dev of rxIssues) {
                console.log(`    - ${dev.addr} (${dev.shortPath}): Received 0 beacons`);
                console.log(`      → Try: mesh reset, check antenna, reposition`);
            }
        }

        // TX issues
        if (txIssues.length > 0) {
            healthy = false;
            console.log('\n  ✗ TX ISSUES (beacons not received by others):');
            for (const dev of txIssues) {
                console.log(`    - ${dev.addr} (${dev.shortPath}): No one received its beacon`);
                console.log(`      → Try: mesh reset, check antenna`);
            }
        }

        // All healthy
        if (healthy) {
            console.log('\n  ✓ MESH HEALTHY');
            console.log('    All devices can communicate with each other');
        }

        // Phase 6: Final peer check
        console.log('\n\nPHASE 6: PEER TABLES');
        console.log('-'.repeat(40));

        for (const dev of devices) {
            if (!dev.port || !dev.addr) continue;
            const peers = await dev.send({ cmd: 'peers' }, 'peers');
            if (peers?.event === 'peers') {
                const peerList = (peers.peers || []).map(p => {
                    const stale = p.stale ? '*' : '';
                    return `${p.addr}${stale}`;
                }).join(', ');
                console.log(`  ${dev.addr}: ${peers.count} peers [${peerList}]`);
            }
        }
        console.log('  (* = stale peer)');
        console.log('');

        // If mesh looks broken, offer reset option
        if (!healthy) {
            console.log('\nWould you like to reset mesh on all devices? (requires reboot after)');
            console.log('Run: node device-diag.js --reset-all\n');
        }

        console.log('='.repeat(60));
        console.log('DIAGNOSTICS COMPLETE');
        console.log('='.repeat(60) + '\n');

    } catch (err) {
        console.error('Error:', err.message);
    } finally {
        for (const dev of devices) {
            dev.close();
        }
    }
}

async function runSingleDeviceDiag(portPath) {
    console.log('\n' + '='.repeat(50));
    console.log('HID-HOP DEVICE DIAGNOSTICS');
    console.log('='.repeat(50));
    console.log(`\nTarget: ${portPath}\n`);

    const dev = new DeviceDiag(portPath);
    const results = {
        pass: [],
        fail: [],
        warn: []
    };

    try {
        // Connect
        console.log('1. CONNECTING...');
        await dev.connect();
        console.log('   ✓ Connected\n');
        results.pass.push('Serial connection');

        // Version check
        console.log('2. FIRMWARE VERSION...');
        const version = await dev.send({ cmd: 'version' }, 'info');
        if (version?.event === 'info' && version.version) {
            console.log(`   ✓ v${version.version} git:${version.git} (${version.build})`);
            results.pass.push('Firmware version');
        } else if (version?.event === 'ack') {
            console.log('   ✗ OLD FIRMWARE - version command not recognized');
            results.fail.push('Firmware too old - missing version command');
        } else {
            console.log('   ⚠ No response to version command');
            results.warn.push('Version command timeout');
        }
        console.log('');

        // Mesh status
        console.log('3. MESH STATUS...');
        const mesh = await dev.send({ cmd: 'mesh_status' }, 'mesh_status');
        if (mesh?.event === 'mesh_status') {
            console.log(`   Address:      ${mesh.addr}`);
            console.log(`   Provisioned:  ${mesh.provisioned}`);
            console.log(`   App Key:      ${mesh.app_key_bound}`);
            console.log(`   Relay:        ${mesh.relay}`);
            console.log(`   Founder:      ${mesh.founder}`);
            console.log(`   Peer Count:   ${mesh.peer_count}`);
            console.log(`   Name:         "${mesh.name}"`);

            if (!mesh.provisioned) {
                results.fail.push('Not provisioned');
            } else {
                results.pass.push('Provisioned');
            }
            if (!mesh.app_key_bound) {
                results.fail.push('App key not bound');
            } else {
                results.pass.push('App key bound');
            }
        } else {
            console.log('   ✗ No mesh status response');
            results.fail.push('Mesh status failed');
        }
        console.log('');

        // Beacon send test
        console.log('4. BEACON SEND TEST...');
        const beacon = await dev.send({ cmd: 'beacon' }, 'beacon');
        if (beacon?.event === 'beacon' && beacon.sent) {
            console.log('   ✓ Beacon sent successfully');
            results.pass.push('Beacon TX');
        } else if (beacon?.event === 'ack') {
            console.log('   ✗ OLD FIRMWARE - beacon command not recognized');
            results.fail.push('Firmware too old - missing beacon command');
        } else {
            console.log('   ⚠ Unexpected beacon response:', JSON.stringify(beacon));
            results.warn.push('Beacon response unexpected');
        }
        console.log('');

        // Peer list
        console.log('5. PEER LIST...');
        const peers = await dev.send({ cmd: 'peers' }, 'peers');
        if (peers?.event === 'peers') {
            console.log(`   Count: ${peers.count}`);
            if (peers.peers && peers.peers.length > 0) {
                for (const p of peers.peers) {
                    const staleStr = p.stale ? ' [STALE]' : '';
                    console.log(`   - ${p.addr} rssi:${p.rssi} age:${p.age_s}s${staleStr}`);
                }

                const staleCount = peers.peers.filter(p => p.stale).length;
                if (staleCount === peers.peers.length && peers.peers.length > 0) {
                    console.log('   ⚠ ALL peers are stale - not receiving beacons?');
                    results.warn.push('All peers stale');
                }
            } else {
                console.log('   ⚠ No peers in list');
                results.warn.push('No peers');
            }
        }
        console.log('');

        // Beacon receive test
        console.log('6. BEACON RECEIVE TEST (15s)...');
        console.log('   Waiting for beacon_rcvd events...');
        console.log('   (Send beacons from other devices now)');
        dev.clearBeaconStats();

        let beaconsReceived = 0;
        const startTime = Date.now();
        while (Date.now() - startTime < 15000) {
            const evt = dev.events.find(e =>
                e.time > startTime && e.data.event === 'beacon_rcvd'
            );
            if (evt) {
                beaconsReceived++;
                console.log(`   ← Beacon from ${evt.data.from} rssi:${evt.data.rssi}`);
                dev.events = dev.events.filter(e => e !== evt);
            }
            await sleep(100);
        }

        if (beaconsReceived > 0) {
            console.log(`   ✓ Received ${beaconsReceived} beacon(s)`);
            results.pass.push('Beacon RX');
        } else {
            console.log('   ✗ No beacons received');
            console.log('   → This device may have RX issues or no nearby nodes');
            results.fail.push('No beacons received');
        }
        console.log('');

        // Discovery test (old-style request/response)
        console.log('7. DISCOVERY TEST (request/response)...');
        dev.events = [];
        const disc = await dev.send({ cmd: 'mesh_discover' }, 'mesh_discover');
        if (disc?.event === 'mesh_discover' && disc.sent) {
            console.log('   ✓ Discovery broadcast sent');
            console.log('   Waiting 5s for responses...');
            await sleep(5000);

            const responses = dev.events.filter(e =>
                e.data.event === 'discovery_resp_rcvd'
            );
            if (responses.length > 0) {
                console.log(`   ✓ Received ${responses.length} discovery response(s)`);
                for (const r of responses) {
                    console.log(`   ← Response from ${r.data.from} rssi:${r.data.rssi}`);
                }
                results.pass.push('Discovery RX');
            } else {
                console.log('   ✗ No discovery responses received');
                results.fail.push('No discovery responses');
            }
        }
        console.log('');

        // Summary
        console.log('='.repeat(50));
        console.log('SUMMARY');
        console.log('='.repeat(50));
        console.log(`\n✓ PASS (${results.pass.length}):`);
        results.pass.forEach(r => console.log(`  - ${r}`));

        if (results.warn.length > 0) {
            console.log(`\n⚠ WARN (${results.warn.length}):`);
            results.warn.forEach(r => console.log(`  - ${r}`));
        }

        if (results.fail.length > 0) {
            console.log(`\n✗ FAIL (${results.fail.length}):`);
            results.fail.forEach(r => console.log(`  - ${r}`));
        }

        console.log('\n');
        if (results.fail.length === 0) {
            console.log('DIAGNOSIS: Device appears healthy');
        } else if (results.fail.includes('Firmware too old - missing beacon command')) {
            console.log('DIAGNOSIS: Needs firmware update - flash latest build');
        } else if (results.fail.includes('No beacons received') && results.pass.includes('Beacon TX')) {
            console.log('DIAGNOSIS: Can transmit but not receive');
            console.log('  → Possible hardware RX issue or mesh config problem');
            console.log('  → Try: reset (mesh reprovision)');
        } else if (results.fail.includes('Not provisioned')) {
            console.log('DIAGNOSIS: Not provisioned - will auto-provision on next boot');
        } else if (results.fail.includes('App key not bound')) {
            console.log('DIAGNOSIS: App key not bound - should self-heal, try reboot');
        }
        console.log('');

    } catch (err) {
        console.error('Error:', err.message);
    } finally {
        dev.close();
    }
}

async function resetAllDevices(portPaths) {
    console.log('\n' + '='.repeat(60));
    console.log('MESH RESET & REBOOT - ALL DEVICES');
    console.log('='.repeat(60));
    console.log(`\nResetting ${portPaths.length} devices...\n`);

    const devices = portPaths.map(p => new DeviceDiag(p));

    try {
        // First reset all
        for (const dev of devices) {
            try {
                await dev.connect();
                const result = await dev.send({ cmd: 'mesh_reset' }, 'mesh_reset');
                if (result?.done) {
                    console.log(`  ✓ ${dev.shortPath} - mesh reset`);
                } else {
                    console.log(`  ⚠ ${dev.shortPath} - unexpected: ${JSON.stringify(result)}`);
                }
            } catch (err) {
                console.log(`  ✗ ${dev.shortPath} - ${err.message}`);
            }
        }

        console.log('\n  Rebooting all devices...\n');

        // Then reboot all
        for (const dev of devices) {
            if (!dev.port) continue;
            try {
                await dev.send({ cmd: 'reboot' }, 'reboot');
                console.log(`  ↻ ${dev.shortPath} - rebooting`);
            } catch (err) {
                // Device may disconnect before we get response
                console.log(`  ↻ ${dev.shortPath} - reboot sent`);
            }
        }

        console.log('\n' + '='.repeat(60));
        console.log('Devices rebooting... wait a few seconds then run:');
        console.log('  node device-diag.js');
        console.log('='.repeat(60) + '\n');

    } finally {
        for (const dev of devices) {
            dev.close();
        }
    }
}

async function main() {
    let portPath = process.argv[2];

    const ports = await SerialPort.list();
    const usbModems = ports.filter(p => p.path.includes('usbmodem'));

    if (usbModems.length === 0) {
        console.error('No devices found');
        process.exit(1);
    }

    // Handle --reset-all flag
    if (portPath === '--reset-all') {
        const allPorts = usbModems.map(p => p.path);
        await resetAllDevices(allPorts);
        return;
    }

    if (!portPath) {
        // No port specified - test ALL devices
        if (usbModems.length === 1) {
            // Only one device, run single device diag
            await runSingleDeviceDiag(usbModems[0].path);
        } else {
            // Multiple devices - run multi-device diagnostic
            const allPorts = usbModems.map(p => p.path);
            await runMultiDeviceDiag(allPorts);
        }
    } else if (portPath === '--help' || portPath === '-h') {
        console.log('HID-HOP Device Diagnostics\n');
        console.log('Usage:');
        console.log('  node device-diag.js                    # Test ALL connected devices');
        console.log('  node device-diag.js /dev/tty.usbmodemX # Test single device');
        console.log('');
        console.log('Available devices:');
        usbModems.forEach((p, i) => console.log(`  [${i}] ${p.path}`));
    } else {
        await runSingleDeviceDiag(portPath);
    }
}

main().catch(console.error);
