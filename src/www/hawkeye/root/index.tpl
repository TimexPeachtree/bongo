%(include|header.tpl)s

<img src="/img/welcome-box.png" class="floaty" alt="Welcome to Bongo" />

<h1>Welcome</h1>
<p>Thank you for choosing Bongo as your calendaring and mail solution.</p>
<p>This screen enables you to briefly overview your system status, and to access commonly and recently used tasks.</p>
<p>Help can be accessed by clicking on the help menu at any time.<br />To get started, either click on one of the menus to the left, or click on one of the common tasks listed below.</p>

<table class="htable" width="100%%" cellspacing="0" >
    <tr style="height: 28px;">
        <td class="hrow">Agent Status</td>
        <td class="drow" style="text-align: center;">Operation normal.</td>
        <td class="hrow" width="16"></td>
    </tr>
    <tr style="height: 28px;">
        <td class="hrow">Bongo Updates</td>
        <td class="drow" style="text-align: center;">
	  <p>Running version: <span tal:content="sw_current">unknown</span> <br />
	  Latest available: <span tal:content="sw_available">unknown</span></p>
	  <p tal:condition="sw_upgrade">Upgrade to new version recommended.</p>
	</td>
        <td class="hrow" width="16"><img src="/img/dialog-warning.png" alt="Warning" tal:condition="sw_upgrade" /></td>
    </tr>
    <tr style="height: 28px;">
        <td class="hrow">System Status</td>
        <td style="text-align: center;">Current processing load: <span tal:content="load">unknown</span></td>
        <td class="hrow" width="16"></td>
    </tr>
    <tr style="height: 28px;">
        <td class="hrow">Memory Usage</td>
        <td style="text-align: center;" tal:content="mem">RAM</td>
        <td class="hrow" width="16"></td>
    </tr>
    <tr style="height: 28px;">
        <td class="hrow">Load</td>
        <td style="text-align: center;">Unknown</td>
        <td class="hrow" width="16"></td>
    </tr>
</table>


%(include|footer.tpl)s
