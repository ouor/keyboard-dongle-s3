// Dongle config tool. Built with the csc.exe bundled in Windows (.NET Framework 4.x) -> tiny exe, no runtime install.
using System;
using System.Drawing;
using System.IO.Ports;
using System.Windows.Forms;

class DongleConfig : Form
{
    readonly ComboBox ports = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 120 };
    readonly Button connect = new Button { Text = "Connect", AutoSize = true };
    readonly TextBox log = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, Font = new Font("Consolas", 9) };
    readonly TextBox input = new TextBox { Dock = DockStyle.Bottom };
    readonly SerialPort serial = new SerialPort { BaudRate = 115200, NewLine = "\n", DtrEnable = true };

    DongleConfig()
    {
        Text = "Dongle Config";
        ClientSize = new Size(520, 360);
        var top = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true };
        var refresh = new Button { Text = "Refresh", AutoSize = true };
        var keys = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 130 };
        keys.Items.AddRange(KeyCodes.Names);  // generated from tool/keycodes.json
        keys.SelectedIndexChanged += (s, e) => { input.AppendText((input.Text.EndsWith(" ") || input.Text.Length == 0 ? "" : " ") + keys.SelectedItem); input.Focus(); };
        top.Controls.AddRange(new Control[] { ports, refresh, connect, new Label { Text = "insert key:", AutoSize = true, Padding = new Padding(0, 6, 0, 0) }, keys });
        Controls.Add(log); Controls.Add(input); Controls.Add(top);

        refresh.Click += (s, e) => RefreshPorts();
        connect.Click += (s, e) => Toggle();
        input.KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter) { Send(input.Text); input.Clear(); e.SuppressKeyPress = true; } };
        serial.DataReceived += (s, e) => { string d = serial.ReadExisting(); BeginInvoke((Action)(() => log.AppendText(d.Replace("\n", "\r\n")))); };
        RefreshPorts();
    }

    void RefreshPorts()
    {
        ports.Items.Clear();
        ports.Items.AddRange(SerialPort.GetPortNames());
        if (ports.Items.Count > 0) ports.SelectedIndex = 0;
    }

    void Toggle()
    {
        try
        {
            if (serial.IsOpen) { serial.Close(); connect.Text = "Connect"; return; }
            serial.PortName = (string)ports.SelectedItem;
            serial.Open();
            connect.Text = "Disconnect";
        }
        catch (Exception ex) { MessageBox.Show(ex.Message, "Serial error"); }
    }

    void Send(string line)
    {
        if (!serial.IsOpen) return;
        log.AppendText("> " + line + "\r\n");
        serial.WriteLine(line);
    }

    [STAThread]
    static void Main() { Application.EnableVisualStyles(); Application.Run(new DongleConfig()); }
}
