class ESPHome extends DeviceService
{
    static serviceName = 'esphome';
    static shortName = 'esp';

    constructor(controller, instance)
    {
        super(controller, 'esphome', instance);
        this.intervals.push(setInterval(function() { this.updateLastSeen(); }.bind(this), 1000));
    }

    updateLastSeen()
    {
        if (this.controller.service != this.service)
            return;

        Object.keys(this.devices).forEach(id =>
        {
            let device = this.devices[id];
            let lastSeen = device.lastSeen ?? device.info?.lastSeen;
            let cell = document.querySelector('tr[data-device="' + this.service + '/' + id + '"] .lastSeen');

            if (!cell || !lastSeen)
                return;

            let value = timeInterval(Date.now() / 1000 - lastSeen);

            if (cell.innerHTML == value)
                return;

            cell.dataset.value = lastSeen;
            cell.innerHTML = value;
        });
    }

    updatePage()
    {
        document.querySelector('#serviceVersion').innerHTML = this.version ? 'ESPHome ' + this.version : '<i>unknown</i>';
    }

    parseMessage(list, message)
    {
        if (list[0] == 'status')
        {
            let check = Object.keys(this.devices).length ? false : true;

            this.names = message.names;
            this.version = message.version;

            message.devices.forEach(device =>
            {
                device.id = device.host;

                if (!device.name)
                    device.name = device.id;

                if (!this.devices[device.id])
                {
                    let item = this.names ? device.name : device.host;

                    this.devices[device.id] = new Device(this.service, device.id);
                    this.controller.socket.subscribe('expose/' + this.service + '/' + item);
                    this.controller.socket.subscribe('device/' + this.service + '/' + item);

                    check = true;
                }
                else if (this.names && this.devices[device.id].info.name != device.name)
                {
                    this.controller.socket.unsubscribe('expose/' + this.service + '/' + this.devices[device.id].info.name);
                    this.controller.socket.unsubscribe('device/' + this.service + '/' + this.devices[device.id].info.name);
                    this.controller.socket.subscribe('expose/' + this.service + '/' + device.name);
                    this.controller.socket.subscribe('device/' + this.service + '/' + device.name);
                }

                this.devices[device.id].info = device;

                if (this.controller.service != this.service || this.devices[device.id] != this.device)
                    return;

                this.showDeviceInfo(this.device);
            });

            Object.keys(this.devices).forEach(id =>
            {
                if (message.devices.filter(device => device.host == id).length)
                    return;

                delete this.devices[id];
                check = true;
            });

            if (this.controller.service == this.service)
            {
                if (check)
                    this.controller.showPage(this.service);

                this.updatePage();
            }

            return;
        }

        super.parseMessage(list, message);
    }

    showPage(data)
    {
        let menu = document.querySelector('.menu');
        let list = data ? data.split('=') : new Array();
        let device;

        menu.innerHTML  = '<span id="list"><i class="mdi-menu"></i> List</span>';
        menu.innerHTML += '<span id="add"><i class="mdi-plus"></i> Add</span>';

        menu.querySelector('#list').addEventListener('click', function() { this.controller.showPage(this.service); }.bind(this));
        menu.querySelector('#add').addEventListener('click', function() { this.showDeviceAdd(); }.bind(this));

        if (list[0] == 'device')
            device = this.devices[list[1]];

        if (device)
            this.showDeviceInfo(device);
        else
            this.showDeviceList();

        this.device = device;
        this.updatePage();
    }

    showDeviceList()
    {
        if (!Object.keys(this.devices).length)
        {
            this.content.innerHTML = '<div class="emptyList">' + this.service + ' devices list is empty</div>';
            return;
        }

        loadHTML('html/esphome/deviceList.html', this, this.content, function()
        {
            let table = this.content.querySelector('.deviceList table');

            Object.keys(this.devices).forEach(id =>
            {
                let device = this.devices[id];
                let row = table.querySelector('tbody').insertRow();

                row.dataset.device = this.service + '/' + device.id;
                row.addEventListener('click', function() { this.controller.showPage(this.service + '?device=' + device.id); }.bind(this));

                for (let i = 0; i < 5; i++)
                {
                    let cell = row.insertCell();

                    switch (i)
                    {
                        case 0:
                            cell.innerHTML = device.info.name;
                            cell.colSpan = 2;
                            break;

                        case 1: cell.innerHTML = '<span class="value">' + device.info.host + '</span>'; cell.classList.add('mobileHidden'); break;
                        case 2: cell.innerHTML = device.info.esphomeVersion ?? ''; cell.classList.add('mobileHidden'); break;
                        case 3: cell.innerHTML = this.parseValue(device.info, 'discovery'); cell.classList.add('center', 'mobileHidden'); break;
                    }
                }
            });

            table.querySelectorAll('th.sort').forEach(cell => cell.addEventListener('click', function() { sortTable(table, this.dataset.index); localStorage.setItem('homedESPHomeSort', this.dataset.index); }));
            sortTable(table, localStorage.getItem('homedESPHomeSort') ?? 0);
            addTableSearch(table, 'devices', 'device', 5, [0, 1]);
        });
    }

    showDeviceAdd()
    {
        loadHTML('html/esphome/deviceAdd.html', this, modal.querySelector('.data'), function()
        {
            modal.querySelector('.add').addEventListener('click', function()
            {
                let host = modal.querySelector('input[name="host"]').value.trim();
                let key  = modal.querySelector('input[name="key"]').value.trim();

                if (!host) { modal.querySelector('input[name="host"]').classList.add('error'); return; }
                if (!key)  { modal.querySelector('input[name="key"]').classList.add('error'); return; }

                let port = parseInt(modal.querySelector('input[name="port"]').value) || 6053;
                let name = modal.querySelector('input[name="name"]').value.trim();
                let cmd  = {action: 'addDevice', host: host, key: key, port: port};

                if (name)
                    cmd.name = name;

                this.serviceCommand(cmd);
                showModal(false);

            }.bind(this));

            modal.querySelector('input[name="host"]').addEventListener('input', function() { this.classList.remove('error'); });
            modal.querySelector('input[name="key"]').addEventListener('input', function() { this.classList.remove('error'); });
            modal.querySelector('.cancel').addEventListener('click', function() { showModal(false); });
            showModal(true, 'input[name="host"]');
        });
    }

    showDeviceEdit(device)
    {
        loadHTML('html/esphome/deviceEdit.html', this, modal.querySelector('.data'), function()
        {
            modal.querySelector('.name').innerHTML = device.info.name;
            modal.querySelector('input[name="name"]').value = device.info.name != device.id ? device.info.name : '';
            modal.querySelector('input[name="name"]').placeholder = device.id;
            modal.querySelector('input[name="discovery"]').checked = device.info.discovery;
            modal.querySelector('input[name="active"]').checked = device.info.active;

            modal.querySelector('.save').addEventListener('click', function()
            {
                this.serviceCommand({...{action: 'updateDevice', device: device.id}, ...formData(modal.querySelector('form'))});
            }.bind(this));

            modal.querySelector('.cancel').addEventListener('click', function() { showModal(false); });
            showModal(true, 'input[name="name"]');
        });
    }
}

_homed_service = ESPHome;
