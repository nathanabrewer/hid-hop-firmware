#!/usr/bin/env node
/**
 * HID-HOP Mesh Encryption Test
 * Tests PIN auth + key exchange + encrypted messaging
 */
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const readline = require('readline');

const port1Path = process.argv[2] || '/dev/tty.usbmodem1201';
const port2Path = process.argv[3] || '/dev/tty.usbmodem1401';

let port1, port2, parser1, parser2;
let peer1Addr, peer2Addr;

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

function sendCmd(port, cmd) {
    const json = JSON.stringify(cmd);
    console.log(`  -> ${json}`);
    port.write(json + '\n');
}

async function waitForEvent(parser, eventType, timeout = 5000) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`Timeout waiting for ${eventType}`)), timeout);
        const handler = (line) => {
            try {
                const msg = JSON.parse(line);
                if (msg.event === eventType) {
                    clearTimeout(timer);
                    parser.off('data', handler);
                    resolve(msg);
                }
            } catch (e) {}
        };
        parser.on('data', handler);
    });
}

async function connectPort(path, name) {
    console.log(`[${name}] Opening ${path}...`);
    const port = new SerialPort({ path, baudRate: 115200 });

    return new Promise((resolve, reject) => {
        port.once('open', () => {
            console.log(`[${name}] Open`);
            port.set({ dtr: true, rts: true }, (err) => {
                if (err) console.error(`[${name}] DTR error:`, err.message);
                else console.log(`[${name}] DTR set`);
            });
            resolve(port);
        });
        port.once('error', (err) => {
            console.error(`[${name}] Failed:`, err.message);
            reject(err);
        });
    });
}

async function connect() {
    console.log('\n=== Connecting to devices ===');

    try {
        port1 = await connectPort(port1Path, 'D1');
        port2 = await connectPort(port2Path, 'D2');

        parser1 = port1.pipe(new ReadlineParser({ delimiter: '\r\n' }));
        parser2 = port2.pipe(new ReadlineParser({ delimiter: '\r\n' }));

        // Log all messages
        parser1.on('data', line => console.log(`[D1] <- ${line}`));
        parser2.on('data', line => console.log(`[D2] <- ${line}`));

        console.log('Both devices connected!\n');

        // Give devices time to send "connected" event
        await sleep(500);
    } catch (err) {
        console.error('Connection failed:', err.message);
        throw err;
    }
}

async function getStatus() {
    console.log('=== Getting mesh status ===');

    sendCmd(port1, { cmd: 'mesh_status' });
    await sleep(500);
    sendCmd(port2, { cmd: 'mesh_status' });
    await sleep(1000);
}

async function discover() {
    console.log('\n=== Running discovery ===');

    sendCmd(port1, { cmd: 'mesh_discover' });
    await sleep(2000);

    sendCmd(port1, { cmd: 'peers' });
    await sleep(500);
    sendCmd(port2, { cmd: 'peers' });
    await sleep(1000);
}

async function testAuth(fromPort, toAddr, pin = '123456') {
    console.log(`\n=== Authenticating to ${toAddr} ===`);
    sendCmd(fromPort, { cmd: 'mesh_auth', addr: toAddr, pin });
    await sleep(1500);
}

async function testKeyExchange(fromPort, toAddr) {
    console.log(`\n=== Key exchange with ${toAddr} ===`);
    sendCmd(fromPort, { cmd: 'key_exchange', addr: toAddr });
    await sleep(1500);
}

async function checkEncryptionStatus(port, addr) {
    console.log(`\n=== Checking encryption status for ${addr} ===`);
    sendCmd(port, { cmd: 'encryption_status', addr });
    await sleep(500);
}

async function sendEncryptedText(fromPort, toAddr, text) {
    console.log(`\n=== Sending encrypted text: "${text}" ===`);
    sendCmd(fromPort, { cmd: 'text_enc', addr: toAddr, text });
    await sleep(1000);
}

async function sendEncryptedHID(fromPort, toAddr, text) {
    console.log(`\n=== Sending encrypted HID type: "${text}" ===`);
    sendCmd(fromPort, { cmd: 'hid_type_enc', addr: toAddr, text });
    await sleep(1000);
}

async function testPlaintextRejection(fromPort, toAddr) {
    console.log(`\n=== Testing plaintext HID rejection ===`);
    sendCmd(fromPort, { cmd: 'hid_type', addr: toAddr, text: 'should be rejected' });
    await sleep(1000);
}

async function interactiveMode() {
    const rl = readline.createInterface({
        input: process.stdin,
        output: process.stdout
    });

    console.log('\n=== Interactive Mode ===');
    console.log('Commands:');
    console.log('  peers                          - List all peers');
    console.log('  auth <addr> [pin]              - Authenticate (default PIN: 123456)');
    console.log('  keyex <addr>                   - Key exchange');
    console.log('  status <addr>                  - Check encryption status');
    console.log('  send <addr> "message"          - Send encrypted text');
    console.log('  type <addr> "text"             - Send encrypted HID type');
    console.log('  led <addr> <id> <on|off>       - Set remote LED (encrypted)');
    console.log('  toggle <addr> <id>             - Toggle remote LED (encrypted)');
    console.log('  blink <addr> <id> [count]      - Blink remote LED (encrypted)');
    console.log('  discover                       - Run discovery');
    console.log('  1 <json>  / 2 <json>           - Raw JSON to device 1 or 2');
    console.log('  q                              - Quit\n');

    rl.on('line', (input) => {
        const trimmed = input.trim();

        if (trimmed === 'q') {
            cleanup();
            process.exit(0);
        }

        // Friendly commands (always use port1 as sender)
        if (trimmed === 'peers') {
            sendCmd(port1, { cmd: 'peers' });
        }
        else if (trimmed === 'discover') {
            sendCmd(port1, { cmd: 'mesh_discover' });
        }
        else if (trimmed.startsWith('auth ')) {
            const parts = trimmed.slice(5).split(' ');
            const addr = parts[0];
            const pin = parts[1] || '123456';
            sendCmd(port1, { cmd: 'mesh_auth', addr, pin });
        }
        else if (trimmed.startsWith('keyex ')) {
            const addr = trimmed.slice(6).trim();
            sendCmd(port1, { cmd: 'key_exchange', addr });
        }
        else if (trimmed.startsWith('status ')) {
            const addr = trimmed.slice(7).trim();
            sendCmd(port1, { cmd: 'encryption_status', addr });
        }
        else if (trimmed.startsWith('send ')) {
            const match = trimmed.match(/^send\s+(0x[0-9a-fA-F]+)\s+["'](.+)["']$/);
            if (match) {
                sendCmd(port1, { cmd: 'text_enc', addr: match[1], text: match[2] });
            } else {
                console.log('Usage: send 0x1c04 "your message"');
            }
        }
        else if (trimmed.startsWith('type ')) {
            const match = trimmed.match(/^type\s+(0x[0-9a-fA-F]+)\s+["'](.+)["']$/);
            if (match) {
                sendCmd(port1, { cmd: 'hid_type_enc', addr: match[1], text: match[2] });
            } else {
                console.log('Usage: type 0x1c04 "text to type"');
            }
        }
        else if (trimmed.startsWith('led ')) {
            const match = trimmed.match(/^led\s+(0x[0-9a-fA-F]+)\s+(\d+)\s+(on|off)$/i);
            if (match) {
                const on = match[3].toLowerCase() === 'on';
                sendCmd(port1, { cmd: 'mesh_gpio_led_enc', addr: match[1], id: parseInt(match[2]), on });
            } else {
                console.log('Usage: led 0x1c04 0 on');
            }
        }
        else if (trimmed.startsWith('toggle ')) {
            const match = trimmed.match(/^toggle\s+(0x[0-9a-fA-F]+)\s+(\d+)$/);
            if (match) {
                sendCmd(port1, { cmd: 'mesh_gpio_toggle_enc', addr: match[1], id: parseInt(match[2]) });
            } else {
                console.log('Usage: toggle 0x1c04 0');
            }
        }
        else if (trimmed.startsWith('blink ')) {
            const match = trimmed.match(/^blink\s+(0x[0-9a-fA-F]+)\s+(\d+)(?:\s+(\d+))?$/);
            if (match) {
                const count = match[3] ? parseInt(match[3]) : 3;
                sendCmd(port1, { cmd: 'mesh_gpio_blink_enc', addr: match[1], id: parseInt(match[2]), count });
            } else {
                console.log('Usage: blink 0x1c04 0 5');
            }
        }
        // Raw JSON commands
        else if (trimmed.startsWith('1 ')) {
            try {
                const cmd = JSON.parse(trimmed.slice(2));
                sendCmd(port1, cmd);
            } catch (e) {
                console.log('Invalid JSON');
            }
        } else if (trimmed.startsWith('2 ')) {
            try {
                const cmd = JSON.parse(trimmed.slice(2));
                sendCmd(port2, cmd);
            } catch (e) {
                console.log('Invalid JSON');
            }
        }
    });
}

function cleanup() {
    if (port1) port1.close();
    if (port2) port2.close();
}

async function main() {
    console.log('HID-HOP Encryption Test');
    console.log('=======================\n');
    console.log(`Device 1: ${port1Path}`);
    console.log(`Device 2: ${port2Path}`);

    try {
        await connect();
        await getStatus();
        await discover();

        // Get peer addresses from user or auto-detect
        const rl = readline.createInterface({
            input: process.stdin,
            output: process.stdout
        });

        rl.question('\nEnter D2 address (e.g., 0x00f1): ', async (addr) => {
            rl.close();

            if (!addr.startsWith('0x')) {
                addr = '0x' + addr;
            }

            console.log(`\nTesting encryption flow D1 -> ${addr}`);

            // Step 1: Authenticate
            await testAuth(port1, addr);

            // Step 2: Key exchange
            await testKeyExchange(port1, addr);

            // Step 3: Check status
            await checkEncryptionStatus(port1, addr);

            // Step 4: Test plaintext rejection
            await testPlaintextRejection(port1, addr);

            // Step 5: Send encrypted text
            await sendEncryptedText(port1, addr, 'Hello encrypted world!');

            // Step 6: Send encrypted HID
            await sendEncryptedHID(port1, addr, 'typed via mesh');

            console.log('\n=== Test complete! ===\n');

            // Enter interactive mode
            await interactiveMode();
        });

    } catch (err) {
        console.error('Error:', err.message);
        cleanup();
        process.exit(1);
    }
}

process.on('SIGINT', () => {
    cleanup();
    process.exit(0);
});

process.on('unhandledRejection', (err) => {
    console.error('Unhandled rejection:', err);
    cleanup();
    process.exit(1);
});

main().catch(err => {
    console.error('Fatal:', err);
    cleanup();
    process.exit(1);
});
