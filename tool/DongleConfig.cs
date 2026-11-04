// Dongle config tool. Built with the csc.exe bundled in Windows (.NET Framework 4.x, C# 5) -> tiny exe, no runtime install.
// Talks the CDC line protocol from README.md: status/scan/pair/forget for the device, list/map/unmap/save for keys.
using System;
using System.Drawing;
using System.IO.Ports;
using System.Linq;
using System.Management;
using System.Windows.Forms;

class DongleConfig : Form
{
    readonly SerialPort serial = new SerialPort { BaudRate = 115200, NewLine = "\n", DtrEnable = true };
    readonly Label conn = new Label { Dock = DockStyle.Top, Height = 24, TextAlign = ContentAlignment.MiddleLeft, Padding = new Padding(6, 0, 0, 0) };
    string rx = "";

    // device tab
    readonly Label devState = new Label { AutoSize = true, Font = new Font(SystemFonts.DefaultFont.FontFamily, 11f, FontStyle.Bold) };
    readonly Button scan = new Button { Text = "스캔", AutoSize = true };
    readonly Button pair = new Button { Text = "선택한 장치 페어링", AutoSize = true };
    readonly Button forget = new Button { Text = "현재 장치 삭제", AutoSize = true };
    readonly ListView found = NewList("이름", 200, "주소", 130, "RSSI", 60, "type", 0);

    // mapping tab
    readonly ListView maps = NewList("#", 40, "Report ID", 80, "리포트", 150, "출력 키", 140);
    readonly Label captured = new Label { AutoSize = true, Text = "리모콘 버튼을 누르세요", Font = new Font("Consolas", 10f), Padding = new Padding(0, 6, 0, 0) };
    readonly ComboBox key = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 140 };
    readonly Button apply = new Button { Text = "매핑 추가/변경", AutoSize = true, Enabled = false };
    readonly Button remove = new Button { Text = "선택 매핑 삭제", AutoSize = true };
    string capRid, capHex;

    // log tab
    readonly TextBox log = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, Font = new Font("Consolas", 9) };
    readonly TextBox input = new TextBox { Dock = DockStyle.Bottom };

    static ListView NewList(params object[] cols)
    {
        var l = new ListView { View = View.Details, FullRowSelect = true, MultiSelect = false, HideSelection = false, Dock = DockStyle.Fill };
        for (int i = 0; i < cols.Length; i += 2) l.Columns.Add((string)cols[i], (int)cols[i + 1]);
        return l;
    }

    static FlowLayoutPanel Row(params Control[] c)
    {
        var p = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(4) };
        p.Controls.AddRange(c);
        return p;
    }

    DongleConfig()
    {
        Text = "Dongle Config";
        ClientSize = new Size(560, 420);
        var tabs = new TabControl { Dock = DockStyle.Fill };

        var devTab = new TabPage("장치");
        devTab.Controls.Add(found);
        devTab.Controls.Add(Row(scan, pair, new Label { AutoSize = true, Padding = new Padding(0, 6, 0, 0), Text = "리모콘을 페어링 모드로 두고 스캔" }));
        devTab.Controls.Add(Row(devState, forget));

        var mapTab = new TabPage("키 매핑");
        key.Items.AddRange(KeyCodes.Names);  // generated from tool/keycodes.json
        mapTab.Controls.Add(maps);
        mapTab.Controls.Add(Row(remove, new Label { AutoSize = true, Padding = new Padding(0, 6, 0, 0), Text = "매핑 안 된 버튼은 원래 키 그대로 나갑니다" }));
        mapTab.Controls.Add(Row(new Label { AutoSize = true, Text = "입력:", Padding = new Padding(0, 6, 0, 0) }, captured, new Label { AutoSize = true, Text = "→", Padding = new Padding(0, 6, 0, 0) }, key, apply));

        var logTab = new TabPage("로그");
        logTab.Controls.Add(log);
        logTab.Controls.Add(input);

        tabs.TabPages.AddRange(new[] { devTab, mapTab, logTab });
        Controls.Add(tabs);
        Controls.Add(conn);

        scan.Click += (s, e) => { found.Items.Clear(); Send("scan 5"); };
        pair.Click += (s, e) =>
        {
            if (found.SelectedItems.Count == 0) return;
            var it = found.SelectedItems[0];
            Send("pair " + it.SubItems[1].Text + " " + it.SubItems[3].Text);
        };
        forget.Click += (s, e) =>
        {
            if (MessageBox.Show("현재 장치의 페어링을 지울까요?", Text, MessageBoxButtons.OKCancel) == DialogResult.OK) Send("forget");
        };
        key.SelectedIndexChanged += (s, e) => apply.Enabled = capHex != null;
        apply.Click += (s, e) => { Send("map " + capRid + " " + capHex + " " + key.SelectedItem); Send("save"); Send("list"); };
        remove.Click += (s, e) =>
        {
            if (maps.SelectedItems.Count == 0) return;
            Send("unmap " + maps.SelectedItems[0].Text); Send("save"); Send("list");
        };
        input.KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter) { Send(input.Text); input.Clear(); e.SuppressKeyPress = true; } };
        serial.DataReceived += (s, e) => { try { string d = serial.ReadExisting(); BeginInvoke((Action)(() => Received(d))); } catch (Exception) { } };

        var timer = new Timer { Interval = 1000 };
        timer.Tick += (s, e) => KeepConnected();
        timer.Start();
        KeepConnected();
    }

    // The dongle's CDC port by USB id (303A:4007, firmware/main/usb.c) - no port picking.
    static string FindPort()
    {
        try
        {
            using (var q = new ManagementObjectSearcher("SELECT Name FROM Win32_PnPEntity WHERE PNPDeviceID LIKE 'USB\\\\VID_303A&PID_4007%' AND Name LIKE '%(COM%'"))
                foreach (ManagementObject o in q.Get())
                {
                    string n = (string)o["Name"];
                    int i = n.LastIndexOf("(COM");
                    return n.Substring(i + 1, n.Length - i - 2);
                }
        }
        catch (Exception) { }
        return null;
    }

    void KeepConnected()
    {
        if (serial.IsOpen && SerialPort.GetPortNames().Contains(serial.PortName)) return;
        if (serial.IsOpen) try { serial.Close(); } catch (Exception) { }
        string p = FindPort();
        if (p == null) { SetConn(false, "동글을 찾는 중... (USB에 꽂아주세요)"); return; }
        try
        {
            serial.PortName = p;
            serial.Open();
            SetConn(true, "동글 연결됨 (" + p + ")");
            rx = "";
            Send("status");
            Send("list");
        }
        catch (Exception ex) { SetConn(false, p + " 열기 실패: " + ex.Message); }
    }

    void SetConn(bool ok, string msg)
    {
        conn.Text = msg;
        conn.BackColor = ok ? Color.FromArgb(220, 245, 220) : Color.FromArgb(250, 235, 210);
        foreach (var b in new Control[] { scan, pair, forget, remove }) b.Enabled = ok;
        if (!ok) devState.Text = "-";
    }

    void Received(string d)
    {
        rx += d;
        int n;
        while ((n = rx.IndexOf('\n')) >= 0)
        {
            string line = rx.Substring(0, n).Trim();
            rx = rx.Substring(n + 1);
            if (line.Length > 0) OnLine(line);
        }
    }

    void OnLine(string line)
    {
        log.AppendText(line + "\r\n");
        string[] w = line.Split(new[] { ' ' }, 5);
        // "ok|evt none|connecting|connected <addr> <name>": status reply or state change
        if ((w[0] == "ok" || w[0] == "evt") && w.Length >= 3 && (w[1] == "none" || w[1] == "connecting" || w[1] == "connected"))
        {
            string[] v = line.Split(new[] { ' ' }, 4);
            string name = v.Length > 3 ? v[3].Trim() : "";
            devState.Text = w[1] == "none" ? "페어링된 장치 없음" : (w[1] == "connected" ? "연결됨: " : "찾는 중: ") + (name != "" ? name : w[2]);
            devState.ForeColor = w[1] == "connected" ? Color.DarkGreen : w[1] == "none" ? Color.Gray : Color.DarkOrange;
        }
        else if (w[0] == "evt" && w.Length >= 2 && w[1] == "scan")
        {
            bool end = w.Length >= 3 && w[2] == "end";
            scan.Enabled = end;
            scan.Text = end ? "스캔" : "스캔 중...";
        }
        else if (w[0] == "dev" && w.Length >= 4)
        {
            found.Items.Add(new ListViewItem(new[] { w.Length > 4 ? w[4] : "(이름 없음)", w[1], w[3], w[2] }));
        }
        else if (w[0] == "map" && w.Length >= 5)
        {
            maps.Items.Add(new ListViewItem(new[] { w[1], w[2], w[3], w[4] }));
        }
        else if (w[0] == "rpt" && w.Length >= 3 && w[2].Trim('0') != "")  // all-zero = button released
        {
            capRid = w[1];
            capHex = w[2];
            captured.Text = "ID " + capRid + " / " + capHex;
            apply.Enabled = key.SelectedItem != null;
            foreach (ListViewItem it in maps.Items) it.Selected = it.SubItems[1].Text == capRid && it.SubItems[2].Text == capHex;
        }
    }

    void Send(string line)
    {
        if (!serial.IsOpen) return;
        if (line == "list") maps.Items.Clear();
        log.AppendText("> " + line + "\r\n");
        try { serial.WriteLine(line); } catch (Exception ex) { log.AppendText("! " + ex.Message + "\r\n"); }
    }

    [STAThread]
    static void Main() { Application.EnableVisualStyles(); Application.Run(new DongleConfig()); }
}
