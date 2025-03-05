const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const constants = require('zigbee-herdsman-converters/lib/constants');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const ota = require('zigbee-herdsman-converters/lib/ota');
const e = exposes.presets;
const ea = exposes.access;

const bind = async (endpoint, target, clusters) => {
    for (const cluster of clusters) {
        await endpoint.bind(cluster, target);
    }
};

const ACCESS_STATE = 0b001, ACCESS_WRITE = 0b010, ACCESS_READ = 0b100;

const device = {
        zigbeeModel: ['DIYRuZ_OTAClient'],
        model: 'DIYRuZ_OTAClient',
        vendor: 'DIYRuZ',
        description: '[UART](https://github.com/koptserg/otaclient)',
        supports: 'battery',
        ota: ota.zigbeeOTA,
        fromZigbee: [
            fz.command_toggle,
            fz.battery,
        ],
        toZigbee: [
            tz.factory_reset,
        ],
        meta: {
            configureKey: 1,
            multiEndpoint: true,
        },
        configure: async (device, coordinatorEndpoint) => {
            const firstEndpoint = device.getEndpoint(1);
            await bind(firstEndpoint, coordinatorEndpoint, [
                'genOnOff',
                'genPowerCfg',
            ]);
        },
        exposes: [
            e.action(['toggle']),
            e.battery(),
            e.battery_voltage(),
        ],
};

module.exports = device;

