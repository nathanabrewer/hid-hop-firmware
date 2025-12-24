#!/usr/bin/env node
/**
 * HID-HOP Serial CLI
 * Interactive command-line interface for HID-HOP devices
 */

const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const readline = require('readline');

async function findPort() {
    const ports = await SerialPort.list();
    const usbModem = ports.find(p => p.path.includes('usbmodem'));
    if (usbModem) return usbModem.path;

    console.log('Available ports:');
    ports.forEach((p, i) => console.log(`  [${i}] ${p.path}`));
    return null;
}

function showHelp() {
    console.log('');
    console.log('=== HID-HOP CLI Commands ===');
    console.log('');
    console.log('Discovery & Status:');
    console.log('  peers                          - List all known peers (with stale status)');
    console.log('  discover                       - Broadcast discovery');
    console.log('  mesh                           - Get mesh status');
    console.log('  ping <addr>                    - Ping a node');
    console.log('  discovery-settings             - Show periodic discovery settings');
    console.log('  discovery-interval <ms>        - Set discovery interval (0=disable)');
    console.log('');
    console.log('Authentication & Encryption:');
    console.log('  auth <addr> [pin]              - Authenticate (default PIN: 123456)');
    console.log('  keyex <addr>                   - Key exchange');
    console.log('  encstatus [addr]               - Check encryption status');
    console.log('');
    console.log('Messaging (encrypted):');
    console.log('  send <addr> <message>          - Send encrypted text');
    console.log('  type <addr> <text>             - Type on remote keyboard');
    console.log('');
    console.log('Remote GPIO (encrypted):');
    console.log('  led <addr> <id> <on|off>       - Set remote LED');
    console.log('  toggle <addr> <id>             - Toggle remote LED');
    console.log('  blink <addr> <id> [count]      - Blink remote LED');
    console.log('');
    console.log('Local GPIO:');
    console.log('  gpio                           - Get local GPIO status');
    console.log('  local-led <id> <on|off>        - Set local LED');
    console.log('  local-toggle <id>              - Toggle local LED');
    console.log('  local-blink <id> [count]       - Blink local LED');
    console.log('');
    console.log('Other:');
    console.log('  name [newname]                 - Get or set node name');
    console.log('  text <addr> <msg>              - Send plaintext (use send for encrypted)');
    console.log('  broadcast <msg>                - Broadcast text to all nodes');
    console.log('  reset                          - Reset mesh provisioning');
    console.log('  <json>                         - Send raw JSON command');
    console.log('  help                           - Show this help');
    console.log('  quit                           - Exit');
    console.log('');
}

async function main() {
    let portPath = process.argv[2];

    if (!portPath) {
        portPath = await findPort();
        if (!portPath) {
            console.error('No port specified and no usbmodem found');
            console.log('Usage: node serial-cli.js /dev/tty.usbmodemXXXX');
            process.exit(1);
        }
    }

    console.log('HID-HOP Serial CLI');
    console.log('==================');
    console.log(`Connecting to ${portPath}...`);

    const port = new SerialPort({
        path: portPath,
        baudRate: 115200
    });

    const parser = port.pipe(new ReadlineParser({ delimiter: '\r\n' }));

    // Handle incoming data
    parser.on('data', (line) => {
        try {
            const data = JSON.parse(line);
            const event = data.event || data.error || 'response';
            const color = data.error ? '\x1b[31m' : '\x1b[32m';
            console.log(`${color}<- ${event}\x1b[0m`, JSON.stringify(data, null, 0));
        } catch {
            console.log('<- (raw)', line);
        }
    });

    port.on('error', (err) => {
        console.error('Serial error:', err.message);
    });

    port.on('open', () => {
        console.log('Connected!');
        // Set DTR to trigger device output
        port.set({ dtr: true, rts: true }, (err) => {
            if (err) console.error('DTR error:', err.message);
        });
        showHelp();
    });

    // CLI input
    const rl = readline.createInterface({
        input: process.stdin,
        output: process.stdout,
        prompt: '> '
    });

    rl.prompt();

    rl.on('line', (line) => {
        const input = line.trim();
        if (!input) {
            rl.prompt();
            return;
        }

        let cmd = null;

        // Parse commands
        if (input === 'quit' || input === 'exit' || input === 'q') {
            port.close();
            process.exit(0);
        }
        else if (input === 'help' || input === '?') {
            showHelp();
        }
        else if (input === 'ping') {
            cmd = { cmd: 'ping' };
        }
        else if (input === 'status') {
            cmd = { cmd: 'status' };
        }
        else if (input === 'mesh') {
            cmd = { cmd: 'mesh_status' };
        }
        else if (input === 'peers') {
            cmd = { cmd: 'peers' };
        }
        else if (input === 'discover') {
            cmd = { cmd: 'mesh_discover' };
        }
        else if (input === 'discovery-settings') {
            cmd = { cmd: 'discovery_settings' };
        }
        else if (input.startsWith('discovery-interval ')) {
            const ms = parseInt(input.slice(19).trim());
            if (isNaN(ms) || ms < 0) {
                console.log('Usage: discovery-interval <ms> (0 to disable)');
            } else {
                cmd = { cmd: 'set_discovery_interval', interval: ms };
            }
        }
        else if (input === 'gpio') {
            cmd = { cmd: 'gpio_status' };
        }
        else if (input === 'reset') {
            cmd = { cmd: 'mesh_reset' };
        }
        else if (input === 'name') {
            cmd = { cmd: 'get_name' };
        }
        else if (input.startsWith('name ')) {
            cmd = { cmd: 'set_name', name: input.slice(5).trim() };
        }
        else if (input.startsWith('ping ')) {
            let addr = input.slice(5).trim();
            if (!addr.startsWith('0x')) addr = '0x' + addr;
            cmd = { cmd: 'mesh_ping', addr };
        }
        else if (input.startsWith('auth ')) {
            const parts = input.slice(5).trim().split(/\s+/);
            let addr = parts[0];
            if (!addr.startsWith('0x')) addr = '0x' + addr;
            const pin = parts[1] || '123456';
            cmd = { cmd: 'mesh_auth', addr, pin };
        }
        else if (input.startsWith('keyex ')) {
            let addr = input.slice(6).trim();
            if (!addr.startsWith('0x')) addr = '0x' + addr;
            cmd = { cmd: 'key_exchange', addr };
        }
        else if (input.startsWith('encstatus')) {
            const addr = input.slice(9).trim();
            if (addr) {
                cmd = { cmd: 'encryption_status', addr: addr.startsWith('0x') ? addr : '0x' + addr };
            } else {
                cmd = { cmd: 'encryption_status' };
            }
        }
        else if (input.startsWith('send ')) {
            const match = input.match(/^send\s+(0x[0-9a-fA-F]+|\w+)\s+(.+)$/);
            if (match) {
                let addr = match[1];
                if (!addr.startsWith('0x')) addr = '0x' + addr;
                cmd = { cmd: 'text_enc', addr, text: match[2] };
            } else {
                console.log('Usage: send 0x1c04 your message here');
            }
        }
        else if (input.startsWith('type ')) {
            const match = input.match(/^type\s+(0x[0-9a-fA-F]+|\w+)\s+(.+)$/);
            if (match) {
                let addr = match[1];
                if (!addr.startsWith('0x')) addr = '0x' + addr;
                cmd = { cmd: 'hid_type_enc', addr, text: match[2] };
            } else {
                console.log('Usage: type 0x1c04 text to type');
            }
        }
        else if (input.startsWith('led ')) {
            const match = input.match(/^led\s+(0x[0-9a-fA-F]+|\w+)\s+(\d+)\s+(on|off)$/i);
            if (match) {
                let addr = match[1];
                if (!addr.startsWith('0x')) addr = '0x' + addr;
                cmd = { cmd: 'mesh_gpio_led_enc', addr, id: parseInt(match[2]), on: match[3].toLowerCase() === 'on' };
            } else {
                console.log('Usage: led 0x1c04 0 on');
            }
        }
        else if (input.startsWith('toggle ')) {
            const match = input.match(/^toggle\s+(0x[0-9a-fA-F]+|\w+)\s+(\d+)$/);
            if (match) {
                let addr = match[1];
                if (!addr.startsWith('0x')) addr = '0x' + addr;
                cmd = { cmd: 'mesh_gpio_toggle_enc', addr, id: parseInt(match[2]) };
            } else {
                console.log('Usage: toggle 0x1c04 0');
            }
        }
        else if (input.startsWith('blink ')) {
            const match = input.match(/^blink\s+(0x[0-9a-fA-F]+|\w+)\s+(\d+)(?:\s+(\d+))?$/);
            if (match) {
                let addr = match[1];
                if (!addr.startsWith('0x')) addr = '0x' + addr;
                const count = match[3] ? parseInt(match[3]) : 3;
                cmd = { cmd: 'mesh_gpio_blink_enc', addr, id: parseInt(match[2]), count };
            } else {
                console.log('Usage: blink 0x1c04 0 5');
            }
        }
        else if (input.startsWith('local-led ')) {
            const match = input.match(/^local-led\s+(\d+)\s+(on|off)$/i);
            if (match) {
                cmd = { cmd: 'gpio_led', id: parseInt(match[1]), on: match[2].toLowerCase() === 'on' };
            } else {
                console.log('Usage: local-led 0 on');
            }
        }
        else if (input.startsWith('local-toggle ')) {
            const match = input.match(/^local-toggle\s+(\d+)$/);
            if (match) {
                cmd = { cmd: 'gpio_toggle', id: parseInt(match[1]) };
            } else {
                console.log('Usage: local-toggle 0');
            }
        }
        else if (input.startsWith('local-blink ')) {
            const match = input.match(/^local-blink\s+(\d+)(?:\s+(\d+))?$/);
            if (match) {
                const count = match[2] ? parseInt(match[2]) : 3;
                cmd = { cmd: 'gpio_blink', id: parseInt(match[1]), count };
            } else {
                console.log('Usage: local-blink 0 5');
            }
        }
        else if (input.startsWith('text ')) {
            const match = input.match(/^text\s+(0x[0-9a-fA-F]+|\w+)\s+(.+)$/);
            if (match) {
                let addr = match[1];
                if (!addr.startsWith('0x')) addr = '0x' + addr;
                cmd = { cmd: 'mesh_text', addr, text: match[2] };
            } else {
                console.log('Usage: text 0x1c04 hello world');
            }
        }
        else if (input.startsWith('broadcast ')) {
            cmd = { cmd: 'mesh_text', text: input.slice(10) };
        }
        else if (input.startsWith('{')) {
            // Raw JSON
            try {
                cmd = JSON.parse(input);
            } catch (e) {
                console.log('Invalid JSON');
            }
        }
        else {
            console.log(`Unknown command: ${input}`);
            console.log('Type "help" for available commands');
        }

        if (cmd) {
            const json = JSON.stringify(cmd);
            console.log(`\x1b[36m-> ${json}\x1b[0m`);
            port.write(json + '\n');
        }

        rl.prompt();
    });

    rl.on('close', () => {
        port.close();
        process.exit(0);
    });
}

process.on('SIGINT', () => {
    process.exit(0);
});

main().catch(console.error);
