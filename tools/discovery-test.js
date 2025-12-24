#!/usr/bin/env node
/**
 * Discovery connectivity test - checks which radios can see each other
 */
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const ports = process.argv.slice(2);
if (ports.length < 2) {
    console.log('Usage: node discovery-test.js /dev/tty.usbmodem1 /dev/tty.usbmodem2 ...');
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
    const json = JSON.stringify(cmd);
    conn.port.write(json + '\n');
}

async function main() {
    console.log('=== HID-HOP Discovery Connectivity Test ===\n');

    // Open all ports
    console.log('Opening ports...');
    for (const path of ports) {
        try {
            const conn = await openPort(path);
            connections.push(conn);
            console.log(`  Opened ${path}`);
        } catch (err) {
            console.error(`  Failed to open ${path}: ${err.message}`);
        }
    }

    if (connections.length < 2) {
        console.error('Need at least 2 ports');
        process.exit(1);
    }

    await sleep(500);

    // Get mesh status from each
    console.log('\nIdentifying radios...');
    for (const conn of connections) {
        conn.parser.on('data', (line) => {
            try {
                const msg = JSON.parse(line);
                if (msg.event === 'mesh_status') {
                    radioInfo[conn.path] = { addr: msg.addr, name: msg.name };
                    console.log(`  ${conn.path} -> ${msg.addr} (${msg.name || 'unnamed'})`);
                }
                if (msg.event === 'discovery_rcvd') {
                    console.log(`  [${radioInfo[conn.path]?.addr || conn.path}] RECEIVED discovery from ${msg.from} (rssi: ${msg.rssi})`);
                }
                if (msg.event === 'discovery_resp_rcvd') {
                    console.log(`  [${radioInfo[conn.path]?.addr || conn.path}] Got response from ${msg.from} (rssi: ${msg.rssi})`);
                }
            } catch {}
        });
        sendCmd(conn, { cmd: 'mesh_status' });
    }

    await sleep(1000);

    // Test discovery from each radio
    for (let i = 0; i < connections.length; i++) {
        const sender = connections[i];
        const senderAddr = radioInfo[sender.path]?.addr || sender.path;

        console.log(`\n--- Testing discovery from ${senderAddr} ---`);
        sendCmd(sender, { cmd: 'mesh_discover' });

        await sleep(2000);
    }

    // Summary
    console.log('\n=== Test Complete ===');
    console.log('Check above for which radios received discovery_rcvd events.');
    console.log('If a radio never shows "RECEIVED discovery from X", it cannot hear X.\n');

    // Cleanup
    for (const conn of connections) {
        conn.port.close();
    }
    process.exit(0);
}

main().catch(err => {
    console.error('Error:', err);
    process.exit(1);
});
