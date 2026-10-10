// 賽車儀表（projects/racedash）的 SimHub 公式。
//
// 用法：SimHub -> Custom Serial devices -> 新增裝置 -> 選 ST-LINK 的 COM 埠、115200
//       -> Update messages 新增一則、勾 Use JavaScript、把這整個檔案貼進去、頻率 10Hz
//
// 公式出自 caa1211/esp32-gt7-dashboard 的 simhub/custom-protocol.txt（MIT License），
// 只改了最後一行：結尾加換行。原專案用的是 SimHub 的 Arduino Custom Protocol
// （SimHub 會自己加換行），這裡改走免費版就有的 Custom Serial devices。
// Revision: DSH1-20260927.5 (prefer live FuelPercent alias; gear filtering is in firmware)
function prop(name) {
    try { return $prop(name); } catch (e) { return null; }
}
function first(names) {
    for (var i = 0; i < names.length; i++) {
        var v = prop(names[i]);
        if (v !== null && v !== undefined && String(v) !== '') return v;
    }
    return null;
}
function numeric(v, min, max, digits) {
    if (v === null || v === undefined || String(v) === '') return '--';
    var n = Number(v);
    return isFinite(n) && n >= min && n <= max ? n.toFixed(digits) : '--';
}
function flag(v) {
    if (v === true || v === 1 || String(v).toLowerCase() === 'true' || v === '1') return '1';
    if (v === false || v === 0 || String(v).toLowerCase() === 'false' || v === '0') return '0';
    return '--';
}
function lap(v) {
    if (v === null || v === undefined) return '--';
    var m = /^(?:(\d+):)?(\d{1,2}):(\d{2})(?:\.(\d+))?$/.exec(String(v));
    if (!m) return '--';
    var minutes = Number(m[1] || 0) * 60 + Number(m[2]);
    if (minutes > 999 || Number(m[3]) >= 60) return '--';
    return (minutes < 10 ? '0' : '') + minutes + ':' + m[3] + '.' + ((m[4] || '') + '000').substring(0, 3);
}
function data(name) {
    return first(['DataCorePlugin.GameData.NewData.' + name, 'DataCorePlugin.GameData.' + name, name]);
}
root.dashSequence = ((root.dashSequence || 0) + 1) % 4294967296;
var gear = data('Gear');
gear = gear === null ? '--' : String(gear).toUpperCase();
if (gear === '-1') gear = 'R';
if (gear === '0') gear = 'N';
if (!/^(N|R|[1-9])$/.test(gear)) gear = '--';
var fuelRaw = data('Fuel');
var fuel = Number(fuelRaw);
var fuelCapacityRaw = first([
    'DataCorePlugin.GameData.NewData.FuelCapacity',
    'DataCorePlugin.GameData.FuelCapacity',
    'DataCorePlugin.GameData.NewData.MaxFuel',
    'DataCorePlugin.GameData.MaxFuel',
    'FuelCapacity', 'MaxFuel'
]);
var fuelCapacity = Number(fuelCapacityRaw);
// SimHub's short FuelPercent alias is the live, normalized value used by its
// own Custom Protocol examples. Some games leave the full GameData copy stale.
var fuelPercentRaw = first([
    'FuelPercent',
    'DataCorePlugin.GameData.NewData.FuelPercent',
    'DataCorePlugin.GameData.FuelPercent'
]);
var calculatedFuelPercent = fuelRaw !== null && fuelCapacityRaw !== null &&
    isFinite(fuel) && isFinite(fuelCapacity) && fuelCapacity > 0
    ? fuel / fuelCapacity * 100 : null;
// Some games expose a FuelPercent alias that stops updating. Prefer the
// percentage calculated from live fuel litres and tank capacity when possible.
var fuelPercent = calculatedFuelPercent !== null ? calculatedFuelPercent : fuelPercentRaw;
var perLapRaw = prop('DataCorePlugin.Computed.Fuel_LitersPerLap');
var perLap = Number(perLapRaw);
var remaining = fuelRaw !== null && perLapRaw !== null && isFinite(fuel) && perLap > 0 ? fuel / perLap : null;
var running = flag(prop('DataCorePlugin.GameRunning'));
return [
    'DSH1', root.dashSequence, running === '1' ? '1' : '0',
    numeric(data('SpeedKmh'), 0, 1500, 0), gear,
    numeric(data('Rpms'), 0, 30000, 0),
    numeric(prop('CarSettings_CurrentDisplayedRPMPercent'), 0, 100, 0),
    numeric(prop('CarSettings_RPMRedLineSetting'), 0, 100, 0),
    lap(data('CurrentLapTime')), lap(data('LastLapTime')), lap(data('BestLapTime')),
    numeric(remaining, 0, 9999, 1),
    numeric(data('Position'), 0, 9999, 0),
    numeric(data('CurrentLap'), 0, 9999, 0),
    numeric(data('TotalLaps'), 0, 9999, 0),
    numeric(fuelPercent, 0, 100, 0),
    numeric(data('Throttle'), 0, 100, 0), numeric(data('Brake'), 0, 100, 0),
    flag(data('TCActive')), flag(data('ABSActive')), flag(data('LapInvalidated')),
    numeric(data('TyreTemperatureFrontLeft'), -100, 1000, 1),
    numeric(data('TyreTemperatureFrontRight'), -100, 1000, 1),
    numeric(data('TyreTemperatureRearLeft'), -100, 1000, 1),
    numeric(data('TyreTemperatureRearRight'), -100, 1000, 1)
].join(';') + '\n';   // Custom Serial devices 不會自動補換行，板子靠換行分行
