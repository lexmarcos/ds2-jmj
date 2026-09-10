namespace Loader
{
    partial class SettingsForm
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
        if (disposing && (components != null))
        {
        components.Dispose();
        }
        base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
        System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(SettingsForm));
        UseSeperateSavesCheckbox = new System.Windows.Forms.CheckBox();
        label1 = new System.Windows.Forms.Label();
        CopySavesButton = new System.Windows.Forms.Button();
        DS2SectionLabel = new System.Windows.Forms.Label();
        PatchPhantomTimersCheckbox = new System.Windows.Forms.CheckBox();
        PhantomTimerSecondsLabel = new System.Windows.Forms.Label();
        PhantomTimerSecondsInput = new System.Windows.Forms.NumericUpDown();
        DS2TimerDescriptionLabel = new System.Windows.Forms.Label();
        ((System.ComponentModel.ISupportInitialize)PhantomTimerSecondsInput).BeginInit();
        SuspendLayout();
        // 
        // UseSeperateSavesCheckbox
        // 
        UseSeperateSavesCheckbox.AutoSize = true;
        UseSeperateSavesCheckbox.Location = new System.Drawing.Point(26, 27);
        UseSeperateSavesCheckbox.Name = "UseSeperateSavesCheckbox";
        UseSeperateSavesCheckbox.Size = new System.Drawing.Size(147, 19);
        UseSeperateSavesCheckbox.TabIndex = 0;
        UseSeperateSavesCheckbox.Text = "Use seperate save files?";
        UseSeperateSavesCheckbox.UseVisualStyleBackColor = true;
        UseSeperateSavesCheckbox.CheckedChanged += SettingChanged;
        // 
        // label1
        // 
        label1.Location = new System.Drawing.Point(45, 49);
        label1.Name = "label1";
        label1.Size = new System.Drawing.Size(448, 97);
        label1.TabIndex = 1;
        label1.Text = resources.GetString("label1.Text");
        // 
        // CopySavesButton
        // 
        CopySavesButton.Location = new System.Drawing.Point(320, 149);
        CopySavesButton.Name = "CopySavesButton";
        CopySavesButton.Size = new System.Drawing.Size(173, 40);
        CopySavesButton.TabIndex = 2;
        CopySavesButton.Text = "Copy Retail Saves to DSOS";
        CopySavesButton.UseVisualStyleBackColor = true;
        CopySavesButton.Click += CopySavesClicked;
        // 
        // DS2SectionLabel
        // 
        DS2SectionLabel.AutoSize = true;
        DS2SectionLabel.Font = new System.Drawing.Font("Segoe UI", 9F, System.Drawing.FontStyle.Bold);
        DS2SectionLabel.Location = new System.Drawing.Point(26, 205);
        DS2SectionLabel.Name = "DS2SectionLabel";
        DS2SectionLabel.Size = new System.Drawing.Size(85, 15);
        DS2SectionLabel.TabIndex = 3;
        DS2SectionLabel.Text = "Dark Souls II";
        // 
        // PatchPhantomTimersCheckbox
        // 
        PatchPhantomTimersCheckbox.AutoSize = true;
        PatchPhantomTimersCheckbox.Location = new System.Drawing.Point(26, 228);
        PatchPhantomTimersCheckbox.Name = "PatchPhantomTimersCheckbox";
        PatchPhantomTimersCheckbox.Size = new System.Drawing.Size(220, 19);
        PatchPhantomTimersCheckbox.TabIndex = 4;
        PatchPhantomTimersCheckbox.Text = "Remove the PvP session time limit?";
        PatchPhantomTimersCheckbox.UseVisualStyleBackColor = true;
        PatchPhantomTimersCheckbox.CheckedChanged += SettingChanged;
        // 
        // PhantomTimerSecondsLabel
        // 
        PhantomTimerSecondsLabel.AutoSize = true;
        PhantomTimerSecondsLabel.Location = new System.Drawing.Point(45, 258);
        PhantomTimerSecondsLabel.Name = "PhantomTimerSecondsLabel";
        PhantomTimerSecondsLabel.Size = new System.Drawing.Size(145, 15);
        PhantomTimerSecondsLabel.TabIndex = 5;
        PhantomTimerSecondsLabel.Text = "Session length (seconds):";
        // 
        // PhantomTimerSecondsInput
        // 
        PhantomTimerSecondsInput.Increment = new decimal(new int[] { 100, 0, 0, 0 });
        PhantomTimerSecondsInput.Location = new System.Drawing.Point(200, 256);
        PhantomTimerSecondsInput.Maximum = new decimal(new int[] { 100000, 0, 0, 0 });
        PhantomTimerSecondsInput.Minimum = new decimal(new int[] { 60, 0, 0, 0 });
        PhantomTimerSecondsInput.Name = "PhantomTimerSecondsInput";
        PhantomTimerSecondsInput.Size = new System.Drawing.Size(90, 23);
        PhantomTimerSecondsInput.TabIndex = 6;
        PhantomTimerSecondsInput.Value = new decimal(new int[] { 4000, 0, 0, 0 });
        PhantomTimerSecondsInput.ValueChanged += SettingChanged;
        // 
        // DS2TimerDescriptionLabel
        // 
        DS2TimerDescriptionLabel.Location = new System.Drawing.Point(45, 285);
        DS2TimerDescriptionLabel.Name = "DS2TimerDescriptionLabel";
        DS2TimerDescriptionLabel.Size = new System.Drawing.Size(448, 48);
        DS2TimerDescriptionLabel.TabIndex = 7;
        DS2TimerDescriptionLabel.Text = "Dark Souls II ends PvP sessions after roughly 12 minutes. This patches the client-side timer in memory so the session keeps running. It has no effect on Dark Souls III.";
        // 
        // SettingsForm
        // 
        AutoScaleDimensions = new System.Drawing.SizeF(7F, 15F);
        AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
        ClientSize = new System.Drawing.Size(528, 345);
        Controls.Add(DS2TimerDescriptionLabel);
        Controls.Add(PhantomTimerSecondsInput);
        Controls.Add(PhantomTimerSecondsLabel);
        Controls.Add(PatchPhantomTimersCheckbox);
        Controls.Add(DS2SectionLabel);
        Controls.Add(CopySavesButton);
        Controls.Add(label1);
        Controls.Add(UseSeperateSavesCheckbox);
        FormBorderStyle = System.Windows.Forms.FormBorderStyle.FixedToolWindow;
        Name = "SettingsForm";
        ShowInTaskbar = false;
        StartPosition = System.Windows.Forms.FormStartPosition.CenterParent;
        Text = "Settings";
        Load += OnLoad;
        ((System.ComponentModel.ISupportInitialize)PhantomTimerSecondsInput).EndInit();
        ResumeLayout(false);
        PerformLayout();
        }

        #endregion

        private System.Windows.Forms.CheckBox UseSeperateSavesCheckbox;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.Button CopySavesButton;
        private System.Windows.Forms.Label DS2SectionLabel;
        private System.Windows.Forms.CheckBox PatchPhantomTimersCheckbox;
        private System.Windows.Forms.Label PhantomTimerSecondsLabel;
        private System.Windows.Forms.NumericUpDown PhantomTimerSecondsInput;
        private System.Windows.Forms.Label DS2TimerDescriptionLabel;
    }
}