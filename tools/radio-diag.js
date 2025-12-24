#!/usr/bin/env node
/**
 * Radio diagnostics - test ping and check mesh relay
 */
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const ports = process.argv.slice(2);
if (ports.length < 2) {
    console.log('Usage: node radio-diag.js /dev/tty.usbmodem1 /dev/tty.usbmodem2 ...');
    process.exit(1);
}

const connections = [];
const radioInfo = {};

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function openPort(path) {
    return new Promise((resolve, reject) => {
        const port = new SerialPort({ path, baudRate: 115200 });
        const parser = port.pipe(new ReadlineParser({ delimiter: '\r\n' }));
        port.once('open', () => {
            port.set({ dtr: true, rts: true });
            resolve({ port, parser, path });
        });
        port.once('error', reject);
    });
}

async function sendCmd(conn, cmd) {
    conn.port.write(JSON.stringify(cmd) + '\n');
}

async function main() {
    console.log('=== HID-HOP Radio Diagnostics ===\n');

    // Open all ports
    for (const path of ports) {
        try {
            const conn = await openPort(path);
            connections.push(conn);
            console.log(`Opened ${path}`);
        } catch (err) {
            console.error(`Failed: ${path}: ${err.message}`);
        }
    }

    await sleep(300);

    // Setup listeners and get info
    for (const conn of connections) {
        conn.events = [];
        conn.parser.on('data', (line) => {
            try {
                const msg = JSON.parse(line);
                conn.events.push(msg);

                if (msg.event === 'mesh_status') {
                    radioInfo[conn.path] = msg.addr;
                }
            } catch {}
        });
        sendCmd(conn, { cmd: 'mesh_status' });
    }

    await sleep(500);

    console.log('\nRadio addresses:');
    for (const conn of connections) {
        console.log(`  ${conn.path} = ${radioInfo[conn.path]}`);
    }

    // Clear peers and rediscover
    console.log('\nClearing peer lists...');
    for (const conn of connections) {
        sendCmd(conn, { cmd: 'clear_peers' });
    }
    await sleep(300);

    // Run discovery 3 times from each radio
    console.log('\nRunning discovery (3 rounds from each)...');
    for (let round = 1; round <= 3; round++) {
        for (const conn of connections) {
            sendCmd(conn, { cmd: 'mesh_discover' });
            await sleep(800);
        }
    }

    await sleep(1000);

    // Get final peer lists
    console.log('\n=== Final Peer Lists ===');
    for (const conn of connections) {
        conn.events = [];
        sendCmd(conn, { cmd: 'peers' });
    }
    await sleep(500);

    for (const conn of connections) {
        const peersEvt = conn.events.find(e => e.event === 'peers');
        if (peersEvt) {
            console.log(`\n${radioInfo[conn.path]} sees ${peersEvt.count} peers:`);
            for (const p of peersEvt.peers || []) {
                console.log(`  ${p.addr} rssi:${p.rssi} stale:${p.stale} age:${p.age_s}s`);
            }
        }
    }

    // Test unicast ping from 0x3eaf to 0x1c04 (should relay through 0x00f1)
    const c3eaf = connections.find(c => radioInfo[c.path] === '0x3eaf');
    const c1c04 = connections.find(c => radioInfo[c.path] === '0x1c04');

    if (c3eaf && c1c04) {
        console.log('\n=== Testing Unicast Ping 0x3eaf -> 0x1c04 ===');
        console.log('(Should be relayed through 0x00f1 if direct fails)');
        c1c04.events = [];
        sendCmd(c3eaf, { cmd: 'mesh_ping', addr: '0x1c04' });
        await sleep(2000);

        const pingRcvd = c1c04.events.find(e => e.event === 'ping_rcvd' || e.event === 'discovery_rcvd');
        if (pingRcvd) {
            console.log('0x1c04 received ping/discovery!');
        } else {
            console.log('0x1c04 did NOT receive ping');
            console.log('Events on 0x1c04:', c1c04.events.map(e => e.event).join(', ') || 'none');
        }
    }

    console.log('\n=== Done ===');
    for (const conn of connections) conn.port.close();
    process.exit(0);
}

main().catch(console.error);
