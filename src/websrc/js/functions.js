const disbl = "disabled";
const chck = "checked";
const classHide = "visually-hidden";
const apiLink = "/api?action=";
const spiner = $('<span>', {
	"role": "status",
	"class": "spinner spinner-border spinner-color spinner-border-sm"
}).css("margin-left", "10px");
const spiner2 = $("<span>", {
	"class": classHide
});

const statusOk = $('<span>', {
	"role": "status",
}).css("margin-left", "10px").text("✅");

const statusFail = $('<span>', {
	"role": "status",
}).css("margin-left", "10px").text("❌");

const zbFwInfoUrl = "https://raw.githubusercontent.com/codm/XZG/zb_fws/ti/manifest.json";

const headerText = ".modal-title";
const headerBtnClose = ".modal-btn-close";
const modalBody = ".modal-body";
const modalBtns = ".modal-footer";

const pages = {
	API_PAGE_ROOT: { num: 0, str: "/" },
	API_PAGE_GENERAL: { num: 1, str: "/general" },
		// API_PAGE_ETHERNET: { num: 2, str: "/ethernet" },
	API_PAGE_NETWORK: { num: 2, str: "/network" },
	API_PAGE_ZIGBEE: { num: 3, str: "/zigbee" },
	API_PAGE_TOOLS: { num: 5, str: "/tools" },
	API_PAGE_MQTT: { num: 7, str: "/mqtt" }
}

const commands = {
	CMD_ZB_ROUTER_RECON: 0,
	CMD_ZB_RST: 1,
	CMD_ZB_BSL: 2,
	CMD_ESP_RES: 3,
	CMD_ADAP_LAN: 4,
	CMD_ADAP_USB: 5,
	CMD_LED_ACT: 6,
	CMD_ZB_FLASH: 7,
	CMD_CLEAR_LOG: 8,
	CMD_ESP_UPD_URL: 9,
	CMD_ZB_CHK_FW: 10,
	CMD_ZB_CHK_HW: 11,
	CMD_ZB_LED_TOG: 12,
	CMD_ESP_FAC_RES: 13,
	CMD_ZB_ERASE_NVRAM: 14,
	CMD_DNS_CHECK: 15,
	CMD_CLIENT_CHECK: 16
}

const api = {
	actions: {
		API_GET_PAGE: 0,
		API_GET_PARAM: 1,
		API_STARTWIFISCAN: 2,
		API_WIFISCANSTATUS: 3,
		API_GET_FILELIST: 4,
		API_GET_FILE: 5,
		API_SEND_HEX: 6,
		API_WIFICONNECTSTAT: 7,
		API_CMD: 8,
		API_GET_LOG: 9,
		API_DEL_FILE: 10 //,
		//API_FLASH_ZB: 11
	},
	pages: pages,
	commands: commands
}

const IconsStatusCodes = {
	OK: 1,
	WARN: 2,
	ERROR: 3
};

let intervalIdUpdateRoot;
let intervalTimeUpdateRoot;

let updateValues = {};


function applyScale(baseScale) {
	const switchScale = 0.8 + baseScale * 0.4; // 0.8 min > 1.2 max > 0.4 diff
	const selectScale = 0.7 + baseScale * 0.3; // 0.7 min > 1.0 max > 0.3 diff

	document.querySelectorAll('label[for="toggleBtn"]').forEach(function (element) {
		element.style.transform = `scale(${switchScale})`;
	});

	const langSelect = document.getElementById('langSel');
	if (langSelect) {
		langSelect.style.transform = `scale(${selectScale})`;
	}
}


function handleResize() {
	if (window.innerWidth <= 767) {
		applyScale(0.8);
		$(".ui_set").removeClass("hstack");
		$(".ui_set").addClass("vstack");
		$(".switch-container").css("margin-left", "");
		$("#pageName").hide();
	} else {
		applyScale(1.0);
		$(".ui_set").removeClass("vstack");
		$(".ui_set").addClass("hstack");
		$(".switch-container").css("margin-left", "0.3em");
		$("#pageName").show();
	}
}

window.addEventListener('resize', handleResize);
document.addEventListener('DOMContentLoaded', function () {
	setTimeout(connectEvents(), 300);
	const savedLang = localStorage.getItem("selected-lang");
	const browserLang = navigator.language ? navigator.language.substring(0, 2) : navigator.userLanguage;
	let preferredLang = savedLang || (languages.some(lang => lang.value === browserLang) ? browserLang : 'en'); // 'en' как fallback

	changeLanguage(preferredLang);
});

function identifyLed(event, element, led) {
	event.preventDefault();

	const offLed = '💡';
	const onLed = '⭕';
	let count = 0;

	function toggleEmoji() {
		element.innerHTML = element.innerHTML === offLed ? onLed : offLed;
		count++;

		if (count < 11) { // Needed changes x 2 + 1
			setTimeout(toggleEmoji, 500);
			return
		}

		element.innerHTML = offLed;
	}

	$.get(apiLink + api.actions.API_CMD + "&cmd=" + api.commands.CMD_LED_ACT + "&act=3&led=" + led, function (data) {
		toggleEmoji();
	}).fail(function () {
		alert(i18next.t('c.ercn'));
	});
}

$(document).ready(function () { //handle active nav
	$("a[href='" + document.location.pathname + "']").addClass('active');
	loadPage(document.location.pathname);

	if (isMobile()) {
		$("#pageContent").removeClass("container");//no containers for mobile
	}

	handleResize();
	handleClicks();
	handleMsg();
});

function zbOta() {
	let file = $("#zbFirmware")[0].files[0];
	let reader = new FileReader();
	let text;
	let hex;
	reader.onload = function (e) {
		if (isHex(reader.result)) {
			text = reader.result;

			text.split("\n").forEach(function (line, index, arr) {
				if (index === arr.length - 1 && line === "") return;
				hex += text.slice(-(text.length - 9), -2).toUpperCase();
				let hexSize = hex.split(" ").length;
				$.get(apiLink + api.actions.API_SEND_HEX + "&hex=" + hex + "&size=" + hexSize, function (data) {
				});
			});
			const hmax = 248;
			let pos = hmax;
			for (let index = 0; index < (hex.length / hmax); index++) {
				pos += hmax;
			}
		} else {
			alert("This file format not suported!");
		}
	}
	reader.readAsText(file);

}

function isHex(txt) {
	var regex = /[0-9A-Fa-f]{21}/g;
	return regex.test(txt);
}

function copyCode() {
	let textArea = $("#generatedFile");
	if (!navigator.clipboard) {
		textArea.focus();
		textArea.select();
		try {
			let successful = document.execCommand('copy');
			let msg = successful ? 'successful' : 'unsuccessful';
		} catch (err) {
		}
	} else {
		navigator.clipboard.writeText(textArea.val());
	}
	$("#clipIco").attr("xlink:href", "icons.svg#clipboard2-check");
}

function generateConfig(params) {
	let result;
	const mist_cfg_txt = `baudrate: ${$("#baud").val()}
# ${i18next.t('p.zi.cfg.dzl')}
  disable_led: false
# ${i18next.t('p.zi.cfg.sopm')}
advanced:
  transmit_power: 20`;
	const ip = window.location.host;
	const port = $("#port").val();
	if (ip == "192.168.1.1") $(".ap-alert").removeClass(classHide);
	switch (params) {
		case "zha":
			result = "socket://" + ip + ":" + port;
			break;
		case "z2m":
			result = `# ${i18next.t('p.zi.cfg.ss')}
serial:
# ${i18next.t('p.zi.cfg.lxzg')}
  port: tcp://${ip}:${port}
  adapter: zstack
  ${mist_cfg_txt}`;
			break;
		case "usb":
			result = `# ${i18next.t('p.zi.cfg.ha')}
# ${i18next.t('p.zi.cfg.lin')}
# ${i18next.t('p.zi.cfg.ss')}
serial:
# ${i18next.t('p.zi.cfg.lxzg')}
  port: ${i18next.t('p.zi.cfg.dp')}
  adapter: zstack
  ${mist_cfg_txt}`;
			break;

		default:
			break;
	}
	$("#generatedFile").val(result);
}

function fillFileTable(files) {
	const icon_file = `<svg class="card_icon file_icon" viewBox="0 0 16 16"><use xlink:href="icons.svg#file" /></svg>`;
	//const icon_del = `<svg class="card_icon del_icon" viewBox="0 0 16 16"><use xlink:href="icons.svg#magic" /></svg>`;
	const icon_del = `❌`;
	files.forEach((elem) => { //.slice(0, files.length - 1)
		if (elem.size > 0) {
			let $row = $("<tr>").appendTo("#filelist")
			$("<td class='col-min-width'>" + icon_file + "</td>").appendTo($row);
			$("<td><a href='#config_file' onClick=\"readFile(event, '" + elem.filename + "');\">" + elem.filename + "</a></td>").appendTo($row);
			$("<td>" + elem.size + "B</td>").appendTo($row);
			$("<td class='text-end col-min-width'><a href='' style='text-decoration: none !important;' onClick=\"delFile(event, '" + elem.filename + "');\">" + icon_del + "</a></td>").appendTo($row);
		}
	});
}

function sendHex() {
	let hex = $("#sendHex").val().toUpperCase();
	let hexSize = hex.split(" ").length;
	$.get(apiLink + api.actions.API_SEND_HEX + "&hex=" + hex + "&size=" + hexSize, function (data) {
		$("#sendHex").val("");
	});
}

function setIconGlow(iconId, state, show = true) {
	const icon = document.getElementById(iconId);
	if (icon) {
		switch (state) {
			case IconsStatusCodes.ERROR:
				color = '#fc0500'; // danger
				break;
			case IconsStatusCodes.WARN:
				color = '#e9cf01'; // warn
				break;
			case IconsStatusCodes.OK:
				color = '#01b464'; // success
				break;
			default:
				color = '#000000'; // black
		}
		icon.style.filter = color ? `drop-shadow(0 0 1px ${color})
                                    drop-shadow(0 0 2px ${color})
                                    drop-shadow(0 0 4px ${color})` : 'none';
		icon.style.backgroundColor = color ? color : 'transparent';
		icon.style.border = color ? `2px solid ${color}` : 'none';
		if (show) {
			icon.classList.remove(classHide);
		} else {
			icon.classList.add(classHide);
		}
	}
}

function loadPage(url) {
	delete updateValues.zbRole;

	if (window.location.pathname !== url) {
		window.history.pushState("", document.title, url);
	}

	switch (url) {
		case api.pages.API_PAGE_ROOT.str:
			apiGetPage(api.pages.API_PAGE_ROOT);
			break;
		case api.pages.API_PAGE_GENERAL.str:
			apiGetPage(api.pages.API_PAGE_GENERAL);//, () => {
			break;
		case api.pages.API_PAGE_MQTT.str:
			apiGetPage(api.pages.API_PAGE_MQTT, () => {
				if ($("#MqttEnable").prop(chck) == false) {
					MqttInputDsbl(true);
				}
			});
			break;
		case api.pages.API_PAGE_NETWORK.str:
			apiGetPage(api.pages.API_PAGE_NETWORK, () => {
				networkStatusUiInit();

				if ($("#ethEnbl").prop(chck)) {
					EthEnbl(true);
				}
				else {
					EthEnbl(false);
				}

				if ($("#ethDhcp").prop(chck)) {
					EthDhcpDsbl(true);
				}
				else {
					EthDhcpDsbl(false);
				}

				if ($("#wifiSsid").val().length > 1) {
					setTimeout(() => {
						$("#collapseWifiPass").collapse("show");
					}, 500);
				}
				/*$.get(apiLink + api.actions.API_GET_PARAM + "&param=coordMode", function (data) {
					if (parseInt(data) != 1) {//not in wifi mode
						$(".card").addClass("card-disabled");
						toastConstructor("wifiDisabled");
					}
				});*/

				if ($("#wifiEnbl").prop(chck)) {
					WifiEnbl(true);
				}
				else {
					WifiEnbl(false);
				}
				if ($("#wifiDhcp").prop(chck)) {
					WifiDhcpDsbl(true);
				} else {
					WifiDhcpDsbl(false);
				}

				if ($("#wgEnable").prop(chck) == false) {
					WgInputDsbl(true);
				}
				if ($("#hnEnable").prop(chck) == false) {
					HnInputDsbl(true);
				}
			});
			break;
		case api.pages.API_PAGE_ZIGBEE.str:
			apiGetPage(api.pages.API_PAGE_ZIGBEE, () => {
				generateConfig("z2m");
				backhaulUiInit();
			});
			break;
		case api.pages.API_PAGE_TOOLS.str:
			apiGetPage(api.pages.API_PAGE_TOOLS, () => {
				if ($("#webAuth").prop(chck)) {
					SeqInputDsbl(false);
				}
				if ($("#fwEnabled").prop(chck)) {
					SeqInputDsblFw(false);
				}

				$.get(apiLink + api.actions.API_GET_FILELIST, function (data) {
					fillFileTable(data.files);
				});
				$.get(apiLink + api.actions.API_GET_PARAM + "&param=refreshLogs", function (data) {
					if (parseInt(data) >= 1) {
						logRefresh(parseInt(data) * 1000);
					} else {
						logRefresh(1000);
					}
				});
			});
			break;
		default:
			apiGetPage(api.pages.API_PAGE_ROOT);
			break;
	}
	if (url != api.pages.API_PAGE_NETWORK.str && $('.toast').hasClass("show")) {
		if ($('#toastBody').text().indexOf("Wi-Fi mode") > 0) {
			$('.toast').toast('hide');
		}
	}
}

function espReboot() {
	$.get(apiLink + api.actions.API_CMD + "&cmd=" + api.commands.CMD_ESP_RES);
}

function localizeTitle(url) {
	let page_title = "";
	switch (url) {
		case api.pages.API_PAGE_ROOT.str:
			page_title = i18next.t('l.st');
			break;
		case api.pages.API_PAGE_GENERAL.str:
			page_title = i18next.t('l.ge');
			break;
		case api.pages.API_PAGE_NETWORK.str:
			page_title = i18next.t('l.ne');
			break;
		case api.pages.API_PAGE_ZIGBEE.str:
			page_title = i18next.t('l.zi');
			break;
		case api.pages.API_PAGE_MQTT.str:
			page_title = i18next.t('l.mq');
			break;
		case api.pages.API_PAGE_TOOLS.str:
			page_title = i18next.t('l.to');
			break;
	}
	$("[data-r2v='pageName']").text(page_title);//update page name
	$("title[data-r2v='pageName']").text(page_title + " - CZC");//update page title
}

function apiGetPage(page, doneCall, loader = true) {
	let animDuration = 0;
	const locCall = doneCall;
	if (loader) {
		animDuration = 200;
		showPreloader(true);
	}
	$("#pageContent").fadeOut(animDuration).load(apiLink + api.actions.API_GET_PAGE + "&page=" + page.num, function (response, status, xhr) {
		if (status == "error") {
		} else {
			if (loader) {
				showPreloader(false);
			}
			if (xhr.getResponseHeader("Authentication") == "ok") $(".logoutLink").removeClass(classHide);
			$("#pageContent").fadeIn(animDuration);

			$("form.saveParams").on("submit", function (e) {
				const $target = $(e.currentTarget);
				e.preventDefault();
				showWifiCreds();
				if (this.id === "netCfg") {
					var ethEnblSw = document.getElementById('ethEnbl').checked;
					var wifiEnblSw = document.getElementById('wifiEnbl').checked;
					if (!ethEnblSw && !wifiEnblSw) {
						toastConstructor("anyNetEnbl");
						setTimeout(function () {
							$('.toast').toast('hide');
						}, 10000);
						return;
					}
				}

				const btn = $target.find("button[type='submit']");
				$(':disabled').each(function (e) {
					$(this).removeAttr('disabled');
				});
				spiner.appendTo(btn);
				spiner2.appendTo(btn);
				btn.prop("disabled", true);
				let data = $(this).serialize() + "&pageId=" + page.num;//add page num
				const target = $target.data('target');
				if(target) {
					data += "&target=" + target
				}

				$.ajax({
					type: "POST",
					url: e.currentTarget.action,
					data: data,
					success: function () {
						if(target == "network" || target == "serialSettings") {
							modalConstructor("saveOk");
						}
					},
					error: function () {
						alert(i18next.t('c.erss'));
					},
					complete: function () {
						spiner.remove();
						spiner2.remove();
						btn.prop("disabled", false);
					}
				});
			});
			$("button").click(function () {
				const btnFail = "btn-cmd-fail";
				const btnSuccess = "btn-cmd-success";

				const jbtn = $(this);
				const cmd = jbtn.attr("data-cmd");
				if (cmd) {
					spiner.appendTo(jbtn);
					//spiner2.appendTo(jbtn);
					jbtn.prop("disabled", true);
					$.get(apiLink + api.actions.API_CMD + "&cmd=" + cmd, function (data) {
						spiner.remove();
						jbtn.prop("disabled", false);
						statusOk.appendTo(jbtn);
						setTimeout(function (jbtn) {
							statusOk.remove();
							if (cmd == 3) {
								modalConstructor("restartWait");
							}
						}, 2000, jbtn);
					}).fail(function () {
						spiner.remove();
						jbtn.prop("disabled", false);
						statusFail.appendTo(jbtn);
						setTimeout(function (jbtn) {
							statusFail.remove();
						}, 2000, jbtn);
					});
				}
			});


			let selectedTimeZone = null;
			if (xhr.getResponseHeader("respValuesArr") !== null) {
				const values = JSON.parse(xhr.getResponseHeader("respValuesArr"));
				for (const property in values) {
					if (property === "timeZoneName") {
						selectedTimeZone = values[property];
						continue;
					}
					if (property === "hwBtnIs") {
						//hwBtnIs
						continue;
					}
					if (property === "hwLedUsbIs") {
						if (values[property]) {
							showDivById('ledsCard');
							showDivById('modeLedBtn');
						}
						continue;
					}
					if (property === "hwLedPwrIs") {
						if (values[property]) {
							showDivById('ledsCard');
							showDivById('pwrLedBtn');
						}
						continue;
					}
				}
				dataReplace(values);
			}
			updateLocalizedContent();

			$('[title]').each(function () {
				var title = $(this).attr('title');
				setTitleAndActivateTooltip(this, title);
			});

			if (xhr.getResponseHeader("respTimeZones") !== null) {
				const zones = JSON.parse(xhr.getResponseHeader("respTimeZones"));
				const $dropdown = $("#timeZoneId");
				$dropdown.empty();

				if (Array.isArray(zones)) {
					zones.forEach(item => {
						let option = new Option(item, item);
						if (item === selectedTimeZone) {
							option.selected = true;
						}
						$dropdown.append(option);
					});
				} else {
					console.error("zones is not an array");
				}
			}

			if (typeof (locCall) == "function") locCall();//callback
		}
	});
}

function showDivById(divId) {
	$('#' + divId).removeAttr('hidden');
}

function getReadableTime(beginTime) {
	let elapsedTime = beginTime; // Прошедшее время в миллисекундах
	let seconds = Math.floor(elapsedTime / 1000); // Конвертируем в секунды
	let minutes = Math.floor(seconds / 60); // Конвертируем в минуты
	let hours = Math.floor(minutes / 60); // Конвертируем в часы
	let days = Math.floor(hours / 24); // Конвертируем в дни

	seconds %= 60;
	minutes %= 60;
	hours %= 24;

	let readableTime = `${days} d ` +
		`${hours < 10 ? '0' : ''}${hours}:` +
		`${minutes < 10 ? '0' : ''}${minutes}:` +
		`${seconds < 10 ? '0' : ''}${seconds}`;

	return readableTime;
}

function setTitleAndActivateTooltip(element, newTitle) {
	if (element) {
		element.setAttribute('data-bs-original-title', newTitle);

		let tooltipInstance = bootstrap.Tooltip.getInstance(element);

		if (tooltipInstance) {
			tooltipInstance.update();
		} else {
			new bootstrap.Tooltip(element, {
				placement: 'bottom',
				boundary: 'viewport'
			});
		}
	}
}

function showCardDrawIcon(property, values) {
	if (property === "ethConn") {
		showDivById("ttEt");
		let status;

		if (values[property] === 1) {
			status = IconsStatusCodes.OK;
		} else {
			status = IconsStatusCodes.ERROR;
		}
		setIconGlow('ethIcon', status);
	}

	if (property === "wifiMode") {
		showDivById("ttWi");
	}

	if (property === "wifiConn") {
		let status = IconsStatusCodes.ERROR;

		const wifiMode = values.wifiMode;

		const wifiConn = values[property];

		if (wifiMode == 1) {
			if (wifiConn == 1) {
				status = IconsStatusCodes.OK;
			}
			else if (wifiConn == 2) {
				status = IconsStatusCodes.WARN;
			}
			else {
				status = IconsStatusCodes.ERROR;
			}
		}
		else if (wifiMode == 2) {
			if (wifiConn == 1) {
				status = IconsStatusCodes.OK;
			}
			else if (wifiConn == 2) {
				status = IconsStatusCodes.WARN;
			}
			else {
				status = IconsStatusCodes.ERROR;
			}
		}
		setIconGlow('wifiIcon', status);
	}

	if (property === "connectedSocketStatus") {
		let status;

		if (values[property] > 0) {
			status = IconsStatusCodes.OK;
		} else {
			status = IconsStatusCodes.ERROR;
		}

		setIconGlow('socketIcon', status);
	}

	if (property === "mqConnect") {
		showDivById("ttMq");
		let status;

		if (values[property] === 1) {
			status = IconsStatusCodes.OK;
		} else {
			status = IconsStatusCodes.WARN;
		}
		setIconGlow('mqttIcon', status);
	}

	if (property === "wgInit") {
		showDivById("ttWg");
		let status;

		const wgInit = values[property];
		const wgConnect = values.wgConnect;

		if (wgInit === 1 && wgConnect === 1) {
			status = IconsStatusCodes.OK;
		} else if (wgInit === wgInit) {
			status = IconsStatusCodes.WARN;
		} else {
			status = IconsStatusCodes.ERROR;
		}
		setIconGlow('vpnIcon', status);
	}
	if (property === "hnInit") {
		showDivById("ttHn");
		let status;

		if (values[property] === 1) {
			status = IconsStatusCodes.OK;
		} else {
			status = IconsStatusCodes.ERROR;
		}
		setIconGlow('vpnIcon', status);
	}

	if (property === "ccMode") {
		const ccMode = getCcModeFromIndex(values[property]);
		let text;

		const configGeneratorWrapper = document.getElementById("configGeneratorWrapper");
		const serialConfigurationWrapper = document.getElementById("serialConfigurationWrapper");
		const routerConfigInformation = document.getElementById("routerConfigInformation");
		const ccModeSwitchWrapper = document.getElementById("ccModeSwitchWrapper");

		if (configGeneratorWrapper) {
			configGeneratorWrapper.classList.add("d-none");
		}
		if (serialConfigurationWrapper) {
			serialConfigurationWrapper.classList.add("d-none");
		}
		if (routerConfigInformation) {
			routerConfigInformation.classList.add("d-none");
		}

		switch (ccMode) {
			case "coordinator":
				text = i18next.t('md.zb.dtc');
				if (configGeneratorWrapper) {
					configGeneratorWrapper.classList.remove("d-none");
				}
				if (serialConfigurationWrapper) {
					serialConfigurationWrapper.classList.remove("d-none");
				}
				break;
			case "router":
				text = i18next.t('md.zb.dtr');
				if (routerConfigInformation) {
					routerConfigInformation.classList.remove("d-none");
				}
				break;
			case "openthread":
				text = i18next.t('md.zb.dtt');
				break;
			default:
				text = "[ERROR] Unknown Mode";
				break;
		}
		document.getElementById("ccMode").innerHTML = text;
	}
}

function updateTooltips() {
	let valueToSet = "";
	if (updateValues.connectedSocketStatus > 0) {
		valueToSet = i18next.t('p.st.zbc.sccy', { count: updateValues.connectedSocketStatus });
	}
	else {
		valueToSet = i18next.t('p.st.zbc.sccn');
	}
	valueToSet = valueToSet + "<br><i>" + getReadableTime(updateValues.uptime - updateValues.connectedSocket) + "</i>"
	let element = document.getElementById('socketIcon');
	setTitleAndActivateTooltip(element, '<b>' + valueToSet + '</b>');

	if (updateValues.ethConn) {
		valueToSet = i18next.t('c.conn');
		valueToSet = valueToSet + "<br><i>" + updateValues.ethIp + "</i>";
	}
	else {
		valueToSet = i18next.t('c.disconn');
	}
	element = document.getElementById('ethIcon');
	setTitleAndActivateTooltip(element, '<b>' + valueToSet + '</b>');

	if (updateValues.wifiConn) {
		valueToSet = i18next.t('c.conn');
		valueToSet = valueToSet + "<br><i>" + updateValues.wifiIp + "</i>";
		valueToSet = valueToSet + "<br>" + updateValues.wifiSsid;
	}
	else {
		valueToSet = i18next.t('c.disconn');
	}
	element = document.getElementById('wifiIcon');
	setTitleAndActivateTooltip(element, '<b>' + valueToSet + '</b>');

	if (updateValues.mqConnect) {
		valueToSet = i18next.t('c.conn');
		valueToSet = valueToSet + "<br><i>" + updateValues.mqBroker + "</i>";
	}
	else {
		valueToSet = i18next.t('c.disconn');
	}
	element = document.getElementById('mqttIcon');
	setTitleAndActivateTooltip(element, '<b>' + valueToSet + '</b>');

	if (updateValues.wgConnect) {
		valueToSet = i18next.t('c.conn');
		//valueToSet = valueToSet + "<br><i>" + updateValues.wifiIp + "</i>";
	}
	else {
		valueToSet = i18next.t('c.disconn');
	}
	element = document.getElementById('vpnIcon');
	setTitleAndActivateTooltip(element, '<b>' + valueToSet + '</b>');

	valueToSet = i18next.t('p.st.dic.du');
	valueToSet = valueToSet + "<br><i>" + getReadableTime(updateValues.uptime) + "</i>";
	element = document.getElementById('clock');
	setTitleAndActivateTooltip(element, '<b>' + valueToSet + '</b>');


}

function extractTime(dateStr) {
	const date = new Date(dateStr);
	let hours = date.getHours().toString();
	let minutes = date.getMinutes().toString().padStart(2, '0');
	let seconds = date.getSeconds().toString().padStart(2, '0');

	let pm = "AM";
	if (localStorage.getItem('clock_format_12h') == 'true') {
		if (hours >= 12) {
			pm = "PM";
		}
		if (hours > 12) {
			hours = hours - 12;
		}
		if (hours == 0) {
			hours = 12;
			pm = "AM";
		}
		return `${hours}:${minutes}:${seconds} ${pm}`;
	}
	else {
		hours = hours.padStart(2, '0');
		return `${hours}:${minutes}:${seconds}`;
	}

}

function dataReplace(values, navOnly = false) {
	var clockButton = document.getElementById('clock');
	if (clockButton) {
		clockButton.textContent = extractTime(values.localTime);
	}

	var baseSelector;

	if (navOnly) {
		baseSelector = "nav.navbar [data-r2v='";
	} else {
		baseSelector = "[data-r2v='";
	}

	for (const property in values) {

		showCardDrawIcon(property, values);
		let $elements = $(baseSelector + property + "']");
		if (property == "zbRole") {
			if (values[property] == 1) {
				document.querySelectorAll('.zfs_coordinator').forEach(card => card.classList.add('selected'));
			} else if (values[property] == 2) {
				document.querySelectorAll('.zfs_router').forEach(card => card.classList.add('selected'));
			} else if (values[property] == 3) {
				document.querySelectorAll('.zfs_thread').forEach(card => card.classList.add('selected'));
			}
		}
		if (property == "espUpdAvail" && values[property] == 1) {
			toastConstructor("espUpdAvail");
		}
		if (property == "zbUpdAvail" && values[property] == 1) {
			toastConstructor("zbUpdAvail");
		}
		if (property == "zbFwSaved" && values[property] == 1) {
			$('td[data-r2v="zigbeeFwRev"]')
                .addClass('fst-italic text-danger')
                .tooltip({title: i18next.t('p.st.zbc.fwer'), placement: 'left'})
				.prepend(`
					<svg xmlns="http://www.w3.org/2000/svg" width="1em" height="1em" viewBox="0 0 24 24"><path fill="currentColor" d="M11 17h2v-6h-2zm1-8q.425 0 .713-.288T13 8t-.288-.712T12 7t-.712.288T11 8t.288.713T12 9m0 13q-2.075 0-3.9-.788t-3.175-2.137T2.788 15.9T2 12t.788-3.9t2.137-3.175T8.1 2.788T12 2t3.9.788t3.175 2.137T21.213 8.1T22 12t-.788 3.9t-2.137 3.175t-3.175 2.138T12 22m0-2q3.35 0 5.675-2.325T20 12t-2.325-5.675T12 4T6.325 6.325T4 12t2.325 5.675T12 20m0-8"/></svg>
				`);
		}
		if (property == "no_eth" && values[property] == 1) {
			$('#ethCfg').hide();
		}

		$elements.map(function () {
			const elemType = $(this).prop('nodeName').toLowerCase();
			let valueToSet = values[property];

			switch (property) {
				case "connectedSocketStatus": //clients
					if (valueToSet) {
						valueToSet = i18next.t('p.st.zbc.sccy', { count: valueToSet });
					}
					else {
						valueToSet = i18next.t('p.st.zbc.sccn');
					}
					break;
				case "espNvsSize":
					updateProgressBar("prgNvs", values.espNvsUsed, 0, valueToSet)
					break;
				case "espFsSize":
					updateProgressBar("prgFs", values.espFsUsed, 0, valueToSet)
					break;
				case "deviceTemp":
					updateProgressBar("prgTemp", valueToSet, 15, 85)
					break;
				case "1wTemp":
					$("#1wBar").removeAttr('hidden');
					updateProgressBar("prgTemp1w", valueToSet, 0, 100)
					break;
				case "wifiRssi":
					updateProgressBar("prgRssi", valueToSet, -105, 0)
					valueToSet = valueToSet + " " + "dBm";
					break;
				case "connectedSocket": // socket time
					valueToSet = getReadableTime(values.uptime - valueToSet);
					break;
				case "uptime": 			// device uptime
					valueToSet = getReadableTime(valueToSet);
					break;
				case "ethSpd":
					if (valueToSet != "noConn") {
						valueToSet = valueToSet + " " + "Mbps";
					}
					break;
				case "espFlashType":
					updateValues[property] = values[property];
					switch (valueToSet) {
						case 1:
							valueToSet = i18next.t('p.st.dic.efti');
							break;
						case 2:
							valueToSet = i18next.t('p.st.dic.efte');
							break;
					}
					break;
				case "operationalMode":
					updateValues[property] = values[property];
					switch (valueToSet) {
						case 0:
							valueToSet = i18next.t('p.st.zbc.opn');
							break;
						case 1:
							valueToSet = i18next.t('p.st.zbc.opu');
							break;
					}
					break;
				case "zbRole":
					updateValues[property] = values[property];
					switch (valueToSet) {
						case 1:
							valueToSet = i18next.t('p.st.zr.c');
							break;
						case 2:
							valueToSet = i18next.t('p.st.zr.r');
							break;
						case 2:
							valueToSet = i18next.t('p.st.zr.t');
							break;
						default:
							break;
					}
					break;
				case "ethDhcp":
				case "wifiDhcp":
					if (valueToSet) {
						valueToSet = i18next.t('c.en');
					}
					else {
						valueToSet = i18next.t('c.dis');
					}
					break;

				case "ethConn":
				case "wifiConn":
				case "mqConnect":
				case "wgConnect":
					if (valueToSet) {
						valueToSet = i18next.t('c.conn');
					}
					else {
						valueToSet = i18next.t('c.disconn');
					}

					break;

				case "wifiMode":
					switch (valueToSet) {
						case 0: // Error
							valueToSet = i18next.t('c.err');
							break;
						case 1:
							valueToSet = i18next.t('p.st.wc.mcl');
							break;
						case 2:
							valueToSet = i18next.t('p.st.wc.map');
							break;
					}
					break;

				case "wifiConn": //rename to wifiStatus
					switch (valueToSet) {
						case 0: // Error
							valueToSet = i18next.t('c.err');
							break;
						case 1: // Connecting / Not Started
							valueToSet = i18next.t('c.connecting');
							break;
						case 2: // Connected / Started
							valueToSet = i18next.t('c.conn');
							break;
					}
					break;

				case "wgInit":
				case "hnInit":
					if (valueToSet) {
						valueToSet = i18next.t('c.init');
					}
					else {
						valueToSet = i18next.t('c.err');
					}
					break;
			}

			if (valueToSet == "noConn") {
				valueToSet = i18next.t('c.nc');
			}

			switch (elemType) {
				case "input":
				case "select":
				case "textarea":
					const type = $(this).prop('type').toLowerCase();
					if (elemType == "input" && (type == "checkbox" || type == "radio")) {
						$(this).prop(chck, values[property]);
					} else {
						$(this).val(values[property]);
					}
					break;
				case "option":
					$(this).prop("selected", true);
					break;
				default:
					$(this).text(valueToSet);
					break;
			}
		});
	}
}

function tglPassView(button) {
	var passwordInput = button.previousElementSibling;
	var svgUseElement = button.querySelector('svg use');
	if (passwordInput.type === "password") {
		passwordInput.type = "text";
		svgUseElement.setAttribute('xlink:href', 'icons.svg#eye-slash-fill');
	} else {
		passwordInput.type = "password";
		svgUseElement.setAttribute('xlink:href', 'icons.svg#eye-fill');
	}
}

function showPreloader(state) {
	if (state) {
		$("#xzgPreloader").removeClass(classHide);
	} else {
		$("#xzgPreloader").addClass(classHide);
	}
}

function toastConstructor(params, text) {
	$("#toastButtons").html("");
	$("#toastHeaderText").text("");
	$("#toastBody").text("");
	switch (params) {
		case "espUpdAvail":
			$("#toastHeaderText").text(i18next.t("ts.esp.upd.tt"));
			$("#toastBody").text("");
			$('<button>', {
				type: "button",
				"class": "btn btn-warning",
				text: i18next.t("c.now"),
				click: function () {
					$('.toast').toast('hide');
					modalConstructor("fetchGitReleases");
				}
			}).appendTo("#toastButtons");
			$('<button>', {
				type: "button",
				"class": "btn btn-outline-success",
				text: i18next.t("c.ltr"),
				click: function () {
					$('.toast').toast('hide');

				}
			}).appendTo("#toastButtons");
			break;
		case "zbUpdAvail":
			$("#toastHeaderText").text(i18next.t("ts.zb.upd.tt"));
			$("#toastBody").text(i18next.t("ts.zb.upd.msg"));
			$('<button>', {
				type: "button",
				"class": "btn btn-warning",
				text: i18next.t("c.now"),
				click: function () {
					$('.toast').toast('hide');
					modalConstructor("flashZB");
				}
			}).appendTo("#toastButtons");
			$('<button>', {
				type: "button",
				"class": "btn btn-outline-success",
				text: i18next.t("c.ltr"),
				click: function () {
					$('.toast').toast('hide');

				}
			}).appendTo("#toastButtons");
			break;
		case "espBetaFb":
			$("#toastHeaderText").text(i18next.t("ts.esp.beta.tt"));
			$("#toastBody").text(i18next.t("ts.esp.beta.msg"));
			$('<button>', {
				type: "button",
				"class": "btn btn-warning",
				text: i18next.t("ts.esp.beta.cnt"),
				click: function () {
					var url = "https://t.me/xzg_fw";
					window.open(url, '_blank');
				}
			}).appendTo("#toastButtons");
			$('<button>', {
				type: "button",
				"class": "btn btn-outline-primary",
				text: i18next.t("c.cl"),
				click: function () {
					$('.toast').toast('hide');
					localStorage.setItem('beta_feedback', 1);
				}
			}).appendTo("#toastButtons");
			break;
		case "anyNetEnbl":
			$("#toastHeaderText").text(i18next.t("ts.esp.ane.tt"));
			$("#toastBody").text(i18next.t("ts.esp.ane.msg"));
			$('<button>', {
				type: "button",
				"class": "btn btn-outline-primary",
				text: i18next.t("c.cl"),
				click: function () {
					$('.toast').toast('hide');
				}
			}).appendTo("#toastButtons");
			break;
		case "noZbFw":
			$("#toastHeaderText").text(i18next.t("ts.zb.nzfa.tt"));
			$("#toastBody").text(text);
			$('<button>', {
				type: "button",
				"class": "btn btn-outline-primary",
				text: i18next.t("c.cl"),
				click: function () {
					$('.toast').toast('hide');
				}
			}).appendTo("#toastButtons");
			setTimeout(function () {
				$('.toast').toast('hide');
			}, 10000);
			break;
		default:
			break;
	}
	$('.toast').toast('show');
}


function closeModal() {
	$("#modal").modal("hide");
}

function restartWait() {
	setTimeout(function () {
		modalConstructor("restartWait");
	}, 1000);
}

function extractVersionFromReleaseTag(url) {
	const regex = /\/releases\/download\/([\d.]+)\//;
	const match = url.match(regex);
	if (match) {
		return match[1];
	} else {
		return null;
	}
}

// HTTP completion and a new boot identify a successful update. SSE is only
// progress feedback: its last packet may be lost while the ESP32 restarts.
var espUpdateCurrent = null;
function espUpdateActive() { return espUpdateCurrent && espUpdateCurrent.active; }
function espUpdateReload() { window.location.reload(); }
function espUpdateFailed(run, reason) {
	if (espUpdateCurrent !== run || !run.active) return;
	run.active = false;
	clearTimeout(run.timer);
	$('#prg').removeClass('progress-bar-animated');
	$('#bar').text(i18next.t('md.esp.fu.errors.' + reason, {
		defaultValue: i18next.t('md.esp.fu.failed')
	})).css('color', 'red');
	$('.modal-footer').empty();
	modalAddCancel();
}
function espUpdateProgress(value) {
	if (!espUpdateActive() || espUpdateCurrent.waiting) return;
	const percent = Math.min(99, Math.max(0, Number(value)));
	if (!Number.isFinite(percent)) return;
	$('#prg').css('width', percent + '%').removeClass('progress-bar-animated');
	$('#bar').text(i18next.t('md.esp.fu.prgs', {per: percent.toFixed(2)}));
}
function espUpdateWait(run, confirmed) {
	if (espUpdateCurrent !== run || !run.active) return;
	run.confirmed = run.confirmed || confirmed;
	if (run.confirmed) {
		$('#prg').css('width', '100%').removeClass('progress-bar-animated');
		$('#bar').text(i18next.t('md.esp.fu.ucr')).css('color', 'green');
	} else {
		$('#bar').text(i18next.t('md.esp.fu.checking'));
	}
	if (run.waiting) return;
	run.waiting = true;
	const deadline = Date.now() + 90000;
	function reload() {
		if (espUpdateCurrent !== run || !run.active) return;
		run.active = false;
		espUpdateReload();
	}
	function poll() {
		if (espUpdateCurrent !== run || !run.active) return;
		if (Date.now() >= deadline) { espUpdateFailed(run, 'reconnect_timeout'); return; }
		$.ajax({url: '/api/esp-update', dataType: 'json', cache: false, timeout: 2000})
		.done(function (status) {
			if (espUpdateCurrent !== run || !run.active) return;
			if (!status || typeof status.boot !== 'string' || !status.boot) return;
			if (status.boot !== run.boot) { reload(); return; }
			if (status.state === 'failed') { espUpdateFailed(run, status.error); return; }
			if (status.state === 'restarting') espUpdateWait(run, true);
		})
		.fail(function (xhr) {
			// A stock GitHub image has no backhaul status API. Its HTTP 404
			// after a confirmed update also proves the old app is gone.
			if (run.confirmed && xhr.status === 404 && espUpdateCurrent === run && run.active) {
				$.ajax({url: '/', cache: false, timeout: 2000}).done(reload);
			}
		})
		.always(function () {
			if (espUpdateCurrent === run && run.active) run.timer = setTimeout(poll, 1500);
		});
	}
	run.timer = setTimeout(poll, 1000);
}
function startEspUpdate(params) {
	if (espUpdateActive()) return;
	const run = {active: true, boot: null, confirmed: false, waiting: false, timer: null};
	espUpdateCurrent = run;
	$('#bar').text(i18next.t('md.esp.fu.wdm'));
	$.ajax({url: '/api/esp-update', dataType: 'json', cache: false, timeout: 4000})
	.done(function (status) {
		if (espUpdateCurrent !== run || !run.active) return;
		if (!status || typeof status.boot !== 'string' || !status.boot) { espUpdateFailed(run, 'invalid_response'); return; }
		run.boot = status.boot;
		if (status.state === 'restarting') { espUpdateWait(run, true); return; }
		let request;
		if (params instanceof FormData) {
			const file = params.get('update');
			if (!(file instanceof File) || !file.size) { espUpdateFailed(run, 'invalid_size'); return; }
			request = {url: '/update?size=' + file.size, type: 'POST', data: params,
				contentType: false, processData: false, dataType: 'json'};
		} else {
			const link = params && params.link;
			request = {url: apiLink + api.actions.API_CMD + '&cmd=' + api.commands.CMD_ESP_UPD_URL +
				(link ? '&url=' + encodeURIComponent(link) : ''), dataType: 'text'};
			$('#bar').text(i18next.t(link ? 'md.esp.fu.vgds' : 'md.esp.fu.lgds',
				{ver: link ? extractVersionFromReleaseTag(link) : ''}));
		}
		request.timeout = 180000;
		$.ajax(request).done(function (result) {
			const value = typeof result === 'string' ? result.trim() : result && result.result;
			if (value === 'esp_updated') espUpdateWait(run, true);
			else espUpdateFailed(run, value || 'invalid_response');
		}).fail(function (xhr) {
			if (xhr.status === 0) espUpdateWait(run, false);
			else espUpdateFailed(run, (xhr.responseJSON || {}).result || 'invalid_response');
		});
	})
	.fail(function () { espUpdateFailed(run, 'status_unavailable'); });
}
function espFlashGitWait(params) { startEspUpdate(params); }

let retryCount = 0;
const maxRetries = 30;
var sourceEvents;
var zbFlashLocal = false;
var zbFlashStage = '';

function connectEvents() {
	if (window.location.pathname.startsWith('/login')) {
		return;
	}

	if (retryCount >= maxRetries && !espUpdateActive()) {
		alert(i18next.t('c.cerp'));
		return;
	}

	sourceEvents = new EventSource('/events', { withCredentials: false, timeout: 200 });
	sourceEvents.addEventListener('open', function (e) {
		retryCount = 0;
	}, false);

	sourceEvents.addEventListener('error', function (e) {
		if (e.target !== sourceEvents || e.target.readyState === EventSource.OPEN) return;
		e.target.close();
		if (!espUpdateActive()) retryCount++;
		setTimeout(function () {
			if (sourceEvents === e.target) connectEvents();
		}, espUpdateActive() ? 1500 : Math.min(1000 * Math.pow(2, Math.min(retryCount - 1, 4)), 15000));
	}, false);

	sourceEvents.addEventListener('root_update', function (e) {
		if (e.data != "finish") {
			Object.assign(updateValues, JSON.parse(e.data));
		} else {
			var navOnly = window.location.pathname != "/";
			dataReplace(updateValues, navOnly);
			updateTooltips();
		}
	});

	sourceEvents.addEventListener('zb.dw', function (e) {
		if (zbFlashLocal) return;
		$('#zbFlshPgsTxt').html(i18next.t('md.esp.fu.dwnl', { per: e.data }));
		$("#zbFlshPrgs").css("width", e.data + '%');
	}, false);

	sourceEvents.addEventListener('zb.fp', function (e) {
		if (zbFlashLocal && zbFlashStage !== 'upload' && zbFlashStage !== 'flash') return;
		zbFlashStage = 'flash';
		$('#zbFlshPgsTxt').html(i18next.t('md.esp.fu.flsh', { per: e.data }));
		$("#zbFlshPrgs").css("width", e.data + '%');
	}, false);

	sourceEvents.addEventListener('zb.nv', function (e) {
		let currentContent = $("#console").val();
		let newContent = currentContent + "\n" + e.data;
		const $console = $("#console");
		$console.val(newContent);
		$console.scrollTop($console[0].scrollHeight)
	}, false);

	sourceEvents.addEventListener('zb.fi', function (e) {
		if (zbFlashLocal && zbFlashStage !== 'upload' && zbFlashStage !== 'flash') return;
		let data = e.data.replaceAll("`", "<br>");

		if (e.data == "startDownload") {
			$("#zbFlshPrgs").removeClass("progress-bar-animated");
			data = i18next.t('md.zg.fu.st');
		}

		if (e.data == "startFlash") {
			zbFlashStage = 'flash';
			data = i18next.t('md.zg.fu.stf');
		}

		if (e.data == "erase") {
			data = i18next.t('md.zg.fu.er');
		}

		if (e.data == "verifyFlash") data = i18next.t("md.zg.fu.verify");

		if (e.data == "finishFlash") {
			// Local uploads finish only after the HTTP handler confirms cleanup.
			if (!zbFlashLocal) finishZbFlash();
			return;
		}

		$("#zbFlshPgsTxt").html(data);

	}, false);

	sourceEvents.addEventListener('zb.ff', function (e) {
		if (zbFlashLocal) return;
		let fileName = fileFromUrl(e.data);
		if (fileName) {
			data = i18next.t('md.zg.fu.f', { file: fileName });
		}
		else {
			let ver = "Unknown";
			if (e.data != 0) {
				ver = e.data;
			}
			data = i18next.t('md.zg.fu.nv', { ver: ver });
			setTimeout(function () {
				espReboot();
				restartWait();
			}, 1250);
		}
		$("#zbFlshPgsTxt").html(data);
	}, false);

	sourceEvents.addEventListener('zb.fe', function (e) {
		if (!zbFlashLocal) failZbFlash(e.data.replaceAll("`", "\n"));
	}, false);


	sourceEvents.addEventListener('esp.fp', function (e) {
		espUpdateProgress(e.data);
	}, false);
	sourceEvents.addEventListener('esp.fi', function (e) {
		if (espUpdateActive() && espUpdateCurrent.boot && e.data === 'restarting') {
			espUpdateWait(espUpdateCurrent, false);
		}
	}, false);
}

function fileFromUrl(url) {
	const urlParts = url.split('/');
	if (urlParts.length > 1) {
		return urlParts[urlParts.length - 1];
	}
	return null;
}

function modalAddSpiner() {
	$('<div>', {
		"role": "status",
		"class": "spinner-border spinner-color",
		append: $("<span>", {
			"class": classHide
		})
	}).appendTo(modalBtns);
}

function reconnectEvents() {
	if (sourceEvents) {
		sourceEvents.close();
		setTimeout(function () {
			connectEvents();
		}, 100);
	} else {
		connectEvents();
	}
}

// The flasher takes exclusive UART ownership when the user confirms the update.
function startZbFlash(link, fwMode) {
	zbFlashLocal = link instanceof File;
	zbFlashStage = '';
	configureZigBeeFlashModal(link, fwMode);
}

function configureZigBeeFlashModal(link, fwMode) {
	const localFile = link instanceof File;
	function configure() {
		reconnectEvents();

		$(modalBtns).html("");
		$(modalBody).html("");

		$("<div>", {
			text: i18next.t("md.esp.fu.wm"),
			class: "my-1 text-sm-center text-danger"
		}).appendTo(modalBody);

		let fileName = localFile ? link.name : fileFromUrl(link);
		$("<div>", {
			text: fileName,
			class: "my-1 text-sm-center"
		}).appendTo(modalBody);
		if (localFile) $('<p>', {text: i18next.t('p.bh.flashNote'), class: 'text-sm-center text-danger'}).appendTo(modalBody);

		modalAddCancel();
		let flashButton = $('<button>', {
			type: "button",
			"class": "btn btn-warning",
			text: i18next.t('c.sure'),
			title: i18next.t("md.esp.fu.wm"),
			disabled: true,
			click: function () {
				clearInterval(checkSourceEventsInterval);
				zbFlashStage = localFile ? 'upload' : 'flash';
				$(modalBtns).html("");
				modalAddSpiner();
				$(modalBody).html("");
				$("<div>", {
					id: "zbFlshPgsTxt",
					text: i18next.t("md.esp.fu.wdm"),
					class: "mb-2 text-sm-center"
				}).appendTo(modalBody);
				$("<div>", {
					"class": "progress",
					append: $("<div>", {
						"class": "progress-bar progress-bar-striped progress-bar-animated",
						id: "zbFlshPrgs",
						style: "width: 100%; background-color: var(--link-color);"
					})
				}).appendTo(modalBody);
				if (localFile) uploadZigbeeFile(link, fwMode);
				else $.get(apiLink + api.actions.API_CMD + "&cmd=" + api.commands.CMD_ZB_FLASH + "&url=" + encodeURIComponent(link) + "&fwMode=" + encodeURIComponent(fwMode))
					.fail(() => failZbFlash(i18next.t('p.bh.flashFailed')));
			}
		}).appendTo(modalBtns);

		let checkSourceEventsInterval = setInterval(function () {
			if (sourceEvents && sourceEvents.readyState === EventSource.OPEN) {
				flashButton.prop('disabled', false);
			} else {
				flashButton.prop('disabled', true);
			}
		}, 100);

		$('#modal').one('hidden.bs.modal', function () {
			clearInterval(checkSourceEventsInterval);
		});
	}
	if (localFile) configure();
	else $.get(apiLink + api.actions.API_CMD + "&cmd=" + api.commands.CMD_DNS_CHECK, configure)
		.fail(() => failZbFlash(i18next.t('p.bh.requestFailed')));
}

function finishZbFlash() {
	zbFlashStage = 'done';
	$('#zbFlshPgsTxt').text(i18next.t('md.zg.fu.fn'));
	$(modalBody).find('.progress').addClass(classHide);
	$(modalBody).css('color', 'green');
	$(modalBtns).empty();
	modalAddClose();
}

function failZbFlash(message, details) {
	zbFlashStage = 'error';
	$(modalBody).text(message).css('color', 'red');
	if (details) $('<pre>', {class: 'small text-start mt-3', text: JSON.stringify(details, null, 2)}).appendTo(modalBody);
	console.error('[Zigbee update]', details || message);
	$(modalBtns).empty();
	modalAddClose();
}

function failZbUpload(reply, httpStatus) {
	const code = reply && typeof reply.result === 'string' && /^[a-z0-9_]{1,64}$/.test(reply.result)
		? reply.result : httpStatus ? 'invalid_response' : 'network_error';
	// Only diagnostic fields: never dump the settings response or the PSK.
	const details = {result: code};
	if (httpStatus) details.http_status = httpStatus;
	if (reply) {
		if (typeof reply.stage === 'string' && /^[a-z_]{1,32}$/.test(reply.stage)) details.stage = reply.stage;
		for (const key of ['expected_bytes', 'received_bytes', 'stored_bytes', 'written_bytes', 'radio_flash_bytes', 'bsl_status'])
			if (Number.isFinite(reply[key])) details[key] = reply[key];
		if (typeof reply.erase_started === 'boolean') details.erase_started = reply.erase_started;
		if (typeof reply.bsl_error === 'string' && /^[a-z_]{1,64}$/.test(reply.bsl_error)) details.bsl_error = reply.bsl_error;
	}
	failZbFlash(i18next.t('md.zg.errors.' + code, {defaultValue: code}), details);
}

function uploadZigbeeFile(file, fwMode) {
	const data = new FormData(); data.append('fwMode', fwMode); data.append('radio', file);
	$.getJSON('/api/backhaul').then(function (config) {
		return $.ajax({url: '/updateZB', type: 'POST', headers: {'X-Backhaul-Token': config.token},
			data: data, processData: false, contentType: false, dataType: 'json',
			xhr: function () {
				const xhr = new window.XMLHttpRequest();
				xhr.upload.addEventListener('progress', function (event) {
					if (!event.lengthComputable || zbFlashStage !== 'upload') return;
					const percent = Math.round(event.loaded * 100 / event.total);
					$('#zbFlshPgsTxt').text(i18next.t('md.zg.fu.upload', {per: percent}));
					$('#zbFlshPrgs').removeClass('progress-bar-animated').css('width', percent + '%');
				});
				return xhr;
			}});
	}).done(function (reply) {
		if (reply && reply.result === 'radio_updated') finishZbFlash();
		else failZbUpload(reply, 200);
	}).fail(function (xhr) {
		failZbUpload(xhr.responseJSON, xhr.status);
	});
}

function modalAddClose() {
	$('<button>', {
		type: "button",
		"class": "btn btn-primary",
		text: i18next.t('c.cl'),
		click: function () {
			closeModal();
		}
	}).appendTo(modalBtns);
}

function modalAddCancel() {
	$('<button>', {
		type: "button",
		"class": "btn btn-primary",
		text: i18next.t('c.cancel'),
		click: function () {
			closeModal();
		}
	}).appendTo(modalBtns);
}

function updateProgressBar(id, current, min, max) {
	var progressBar = document.getElementById(id);
	var width = ((current - min) / (max - min)) * 100;
	progressBar.style.width = width + '%';

	var cssVarColorOk = getComputedStyle(document.documentElement)
		.getPropertyValue('--bs-success').trim();
	var cssVarColorWarn = getComputedStyle(document.documentElement)
		.getPropertyValue('--bs-warning').trim();
	var cssVarColorErr = getComputedStyle(document.documentElement)
		.getPropertyValue('--bs-danger').trim();

	var invert = false;
	if (id == "prgRssi") {
		invert = true;
	}

	if ((invert && width > 65) || (!invert && width < 50)) {
		progressBar.style.backgroundColor = cssVarColorOk;
	} else if ((invert && width > 30) || (!invert && width < 80)) {
		progressBar.style.backgroundColor = cssVarColorWarn;
	} else {
		progressBar.style.backgroundColor = cssVarColorErr;
	}
}


function findAllVersionsSorted(data, chip) {
	const categories = ['router', 'coordinator', 'thread'];
	const result = {};

	const chipMap = { "CC2652P2_launchpad": "CC2652P2_launchpad", "CC2652P2_other": "CC2652P2_other", "CC2652P7": "CC2652P7", "CC2652RB": "CC2652RB" };
	let deviceName = chipMap[chip];
	if (!deviceName) {
		//throw new Error("Unsupported chip type or deviceName not set.");
		console.error("error with ZB chip detect");
		deviceName = chip;
	}

	categories.forEach(category => {
		if (data[category]) {
			Object.keys(data[category]).forEach(subCategory => {
				if (subCategory.startsWith(deviceName)) {
					Object.keys(data[category][subCategory]).forEach(file => {
						const fileInfo = data[category][subCategory][file];
						if (!result[category]) {
							result[category] = [];
						}
						result[category].push({
							file: file,
							ver: fileInfo.ver,
							link: fileInfo.link,
							notes: fileInfo.notes,
							baud: fileInfo.baud
						});
					});
				}
			});
		}
	});

	for (const category in result) {
		result[category].sort((a, b) => b.ver - a.ver);
	}

	return result;
}

function getCcModeFromIndex(index) {
	const deviceTypeToFwMap = {
		1: "coordinator",
		2: "router",
		3: "thread"
	};
	return deviceTypeToFwMap[index];
}

// Function definition outside the switch-case
// creates the Webinterface block on System -> Firmware -> Zigbee -> Show available...
function createReleaseBlock(file, deviceType) {
	deviceType = getCcModeFromIndex(deviceType);

	let deviceName;
	let deviceIcon;
	let buttonClass;

	if (deviceType == "coordinator") {
		deviceName = i18next.t('md.zb.dtc');
		buttonClass = "btn btn-outline-danger";
		deviceIcon = "📡";
	} else if (deviceType == "router") {
		deviceName = i18next.t('md.zb.dtr');
		buttonClass = "btn btn-outline-success";
		deviceIcon = "🛰️";
	} else if (deviceType == "thread") {
		deviceName = i18next.t('md.zb.dtt');
		buttonClass = "btn btn-outline-primary";
		deviceIcon = "🚀";
	}

	const uniqueId = 'release-' + Math.random().toString(36).substr(2, 9);

	const releaseBlock = $("<div>", { "class": "release-block", "style": "margin-bottom: 20px;" });
	const headerAndButtonContainer = $('<div>', { "class": "d-flex justify-content-between align-items-start" }).appendTo(releaseBlock);

	const emojiBlock = $('<span>', { "text": deviceIcon }).css('margin-right', '5px').appendTo(headerAndButtonContainer);
	const header = $("<h5>", { "id": uniqueId + '-header', "class": "mb-0", "text": file.ver, "style": "cursor: pointer;" }).appendTo(headerAndButtonContainer);

	setTitleAndActivateTooltip(emojiBlock[0], deviceName);
	setTitleAndActivateTooltip(header[0], i18next.t('md.zb.cte'));

	const buttonContainer = $('<div>', { "class": "d-flex align-items-start" }).appendTo(headerAndButtonContainer);
	const button = $('<a>', {
		"class": buttonClass,
		"click": function () {
			startZbFlash(file.link + "?b=" + file.baud, deviceType);
			let tooltipInstance = bootstrap.Tooltip.getInstance(this);
			if (tooltipInstance) {
				tooltipInstance.hide();
			}
		},
		"data-bs-toggle": "tooltip",
		"title": file.link,
		"text": i18next.t('c.inst'),
		"role": "button"
	}).css("white-space", "nowrap").appendTo(buttonContainer);

	setTitleAndActivateTooltip(button[0], file.link);

	let descriptionDiv;

	if (file.notes.endsWith('.md')) {
		$.get(file.notes, function (data) {
			descriptionDiv = $("<div>", { "id": uniqueId + '-description', "class": "mt-2 release-description", "html": marked.parse(data), "style": "display: none;" }).appendTo(releaseBlock);
			$("<hr>").appendTo(releaseBlock);
		});
	} else {
		descriptionDiv = $("<div>", { "id": uniqueId + '-description', "class": "mt-2 release-description", "html": marked.parse(file.notes), "style": "display: none;" }).appendTo(releaseBlock);
		$("<hr>").appendTo(releaseBlock);
	}

	$(document).on("click", `#${uniqueId}-header`, function () {
		$(`#${uniqueId}-description`).toggle();
	});

	return releaseBlock;
}


function modalConstructor(type, params) {
	const headerText = ".modal-title";
	const modalBody = ".modal-body";
	const modalBtns = ".modal-footer";

	$(headerText).text("").css("color", "");
	$(modalBody).empty().css({ color: "", maxHeight: "400px", overflowY: "auto" });
	$(modalBtns).html("");
	switch (type) {
		case "flashESP":
			$.get(apiLink + api.actions.API_CMD + "&cmd=" + api.commands.CMD_DNS_CHECK);
			$(headerText).text(i18next.t('md.esp.fu.tt')).css("color", "red");
			if (params instanceof FormData) {
				$(modalBody).html(i18next.t("md.esp.fu.lfm"));
			}
			else if (params && 'link' in params && typeof params.link === 'string' && /^https?:\/\/.*/.test(params.link)) {
				$(modalBody).html(i18next.t("md.esp.fu.gvm", { ver: params.ver }));
			} else {
				$(modalBody).html(i18next.t("md.esp.fu.glm"));
			}
			$("<div>", {
				text: i18next.t("md.esp.fu.wm"),
				class: "my-1 text-sm-center text-danger"
			}).appendTo(modalBody);

			modalAddCancel();
			$('<button>', {
				type: "button",
				"class": "btn btn-warning",
				text: i18next.t('c.sure'),
				title: i18next.t("md.esp.fu.wm"),
				click: function () {
					$(modalBtns).html("");
					modalAddSpiner();
					$(modalBody).html("");
					$("<div>", {
						id: "bar",
						text: i18next.t("md.esp.fu.wdm"),
						class: "mb-2 text-sm-center"
					}).appendTo(modalBody);
					$("<div>", {
						class: "progress",
						append: $("<div>", {
							"class": "progress-bar progress-bar-striped progress-bar-animated",
							id: "prg",
							style: "width: 100%; background-color: var(--link-color);"
						})
					}).appendTo(modalBody);
					startEspUpdate(params);
				}
			}).appendTo(modalBtns);
			break;
		case "fetchGitReleases":
			$(headerText).text(i18next.t('md.esp.fu.tt'));
			$(modalBody).html(i18next.t('md.esp.fu.fri'));
			modalAddSpiner();

			fetchReleaseData().then(t => {
				modalConstructor("espGitVersions", {
					releases: t
				})
			}).catch(t => {
				console.error("Failed to fetch release data:", t)
			})
			break;
		case "flashZBM":
			$(headerText).text(i18next.t('md.zb.ot')).css("color", "red");
			break;

		case "flashZB":
			$(headerText).text(i18next.t('md.zb.ot')).css("color", "red");

			$(modalBody).html(i18next.t('md.zb.rfm'));
			modalAddSpiner();

			$.get(zbFwInfoUrl).then(data => {
				const json = JSON.parse(data);
				return $.get(apiLink + api.actions.API_GET_PARAM + "&param=zbHwVer").then(chip => {
					return findAllVersionsSorted(json, chip);
				});
			}).then(fw => {
				$(modalBody).html("");
				$(modalBtns).html("");
				modalAddClose();

				if (fw.coordinator && fw.coordinator.length > 0) {
					fw.coordinator.forEach(file => createReleaseBlock(file, 1).appendTo(".modal-body"));
				} else {
					$("<div>", { "text": i18next.t('md.zb.ncf'), "class": "alert alert-warning" }).appendTo(".modal-body");
				}

				if (fw.router && fw.router.length > 0) {
					fw.router.forEach(file => createReleaseBlock(file, 2).appendTo(".modal-body"));
				} else {
					$("<div>", { "text": i18next.t('md.zb.nrf'), "class": "alert alert-warning" }).appendTo(".modal-body");
				}

				if (fw.thread && fw.thread.length > 0) {
					fw.thread.forEach(file => createReleaseBlock(file, 3).appendTo(".modal-body"));
				} else {
					$("<div>", { "text": i18next.t('md.zb.ntf'), "class": "alert alert-warning" }).appendTo(".modal-body");
				}

			}).fail(error => {
				$(modalBody).html(i18next.t('md.zb.efr')).css("color", "red");
				$(modalBtns).html("");
				modalAddClose();
				console.error(error);
			});

			break;
		case "factoryResetWarning":
			$(headerText).text(i18next.t('md.esp.fr.tt')).css("color", "red");
			$(modalBody).text(i18next.t('md.esp.fr.msg')).css("color", "red");
			modalAddCancel();
			$('<button>', {
				type: "button",
				"class": "btn btn-danger",
				text: i18next.t('c.sure'),
				click: function () {
					$.get(apiLink + api.actions.API_CMD + "&cmd=" + api.commands.CMD_ESP_FAC_RES + "&conf=1", function () {
					});
					modalConstructor("restartWait");
				}
			}).appendTo(modalBtns);
			break;
		case "espGitVersions":
			$(headerText).text(i18next.t('md.esp.fu.gvt'));
			params.releases.forEach(release => {
				const releaseBlock = $("<div>", { "class": "release-block", "style": "margin-bottom: 20px;" });
				const headerAndButtonContainer = $('<div>', {
					"class": "d-flex justify-content-between align-items-start"
				}).appendTo(releaseBlock);
				$("<h5>", {
					"class": "mb-0",
					"text": release.tag_name
				}).appendTo(headerAndButtonContainer);

				const buttonContainer = $('<div>', {
					"class": "d-flex align-items-start"
				}).appendTo(headerAndButtonContainer);

				if (release.assets.length > 0) {
					const downloadLink = release.assets[1].browser_download_url;
					$('<a>', {
						"class": "btn btn-outline-warning",
						click: function () {
							var params = {};
							params['link'] = downloadLink;
							params['ver'] = release.tag_name;
							modalConstructor("flashESP", params);
						},
						"data-bs-toggle": "tooltip",
						"title": i18next.t('c.inst') + " " + release.tag_name,
						"text": i18next.t('c.inst'),
						"role": "button"
					}).css("white-space", "nowrap")
						.appendTo(buttonContainer);
				}
				const releaseDescriptionHtml = marked.parse(release.body);
				$("<div>", {
					"class": "mt-2 release-description",
					"html": releaseDescriptionHtml
				}).appendTo(releaseBlock);
				$("<hr>").appendTo(releaseBlock);
				releaseBlock.appendTo(".modal-body");
			});
			modalAddClose();
			$('<button>', {
				type: "button",
				"class": "btn btn-warning",
				text: i18next.t('p.to.ilfg'),
				click: function () {
					modalConstructor("flashESP");
				}
			}).appendTo(modalBtns);
			break;
		case "restartWait":
			$(headerText).text(i18next.t('md.esp.rst.tt'));
			$(modalBody).html(i18next.t('md.esp.rst.msg'));
			$('<div>', {
				"role": "status",
				"class": "spinner-border spinner-color",
				append: $("<span>", {
					"class": classHide
				})
			}).appendTo(modalBtns);
			var waitTmr = setInterval(function () {
				$.ajax({
					url: "/",
					method: "GET",
					cache: false,
					timeout: 2000,
					success: function () {
						clearInterval(waitTmr);
						clearTimeout(timeoutTmr);
						closeModal();
						window.location = "/";
					}
				});
			}, 3000);
			var timeoutTmr = setTimeout(function () {
				clearInterval(waitTmr);
				$(modalBtns).html("");
				$(modalBody).text(i18next.t('md.esp.rst.nrps')).css("color", "red");
				$('<button>', {
					type: "button",
					"class": "btn btn-warning",
					text: i18next.t('c.cl'),
					click: function () {
						closeModal();
					}
				}).appendTo(modalBtns);
			}, 60000);
			break;
		case "saveOk":
			$.get(apiLink + api.actions.API_GET_PARAM + "&param=wifiEnable", function (wifiEnable) {
				if (window.location.pathname == "/network" & wifiEnable) {
					$(headerText).text(i18next.t('md.esp.ws.tt'));
					$(modalBody).text(i18next.t('md.esp.ws.msg'));
					$('<div>', {
						"role": "status",
						"class": "spinner-border spinner-color",
						append: $("<span>", {
							"class": classHide
						})
					}).appendTo(modalBtns);
					let counter = 0;
					var getWifiIp = setInterval(function (params) {
						if (counter <= 15) {
							$.get(apiLink + api.actions.API_WIFICONNECTSTAT, function (data) {
								if (data.connected) {
									espReboot();
									clearInterval(getWifiIp);
									setTimeout(() => {//5sec for reboot
										$(".modal-body").html(`<span style="color: green">${i18next.t('c.conn')}!</span><br>${i18next.t('md.esp.ws.nip', { ip: data.ip })}`);
										$(modalBtns).html("");
										$('<button>', {
											type: "button",
											"class": "btn btn-success",
											text: i18next.t('md.esp.ws.btn') + " " + data.ip,
											click: function () {
												window.location = "http://" + data.ip + "/";
											}
										}).appendTo(modalBtns);
									}, 3000);
								} else {
									counter++;
								}
							});
						} else {
							clearInterval(getWifiIp);
							$(modalBody).text(i18next.t('md.esp.ws.err')).css("color", "red");
							$(modalBtns).html("");
							$('<button>', {
								type: "button",
								"class": "btn btn-success",
								text: i18next.t('c.cl'),
								click: function () {
									closeModal();
								}
							}).appendTo(modalBtns);
						}
					}, 1000);
				} else {
					let body = i18next.t('md.ss.msg');
					$(headerText).text(i18next.t('md.ss.tt'));
					body += i18next.t('md.ss.rr');
					$('<button>', {
						type: "button",
						"class": "btn btn-warning",
						text: i18next.t('md.ss.rl'),
						click: function () {
							closeModal();
						}
					}).appendTo(modalBtns);
					$('<button>', {
						type: "button",
						"class": "btn btn-primary",
						text: i18next.t('md.ss.rn'),
						click: function () {
							closeModal();
							espReboot();
							restartWait();
						}
					}).appendTo(modalBtns);
					//}
					$(modalBody).text(body);
				}
			});
			break;
		default:
			break;
	}
	$("#modal").modal("show");
}

function showWifiCreds() {
	$("#collapseWifiPass").collapse("show");
}
function getWifiList() {
	$("#collapseWifiPass").collapse("hide");
	$("#wifiScanPreloader").removeClass(classHide);
	$("#wifiScanButton").addClass(classHide);
	WifiEnbl(true);
	$.get(apiLink + api.actions.API_STARTWIFISCAN, function (data) { //visually-hidden wifiLoadSpinner
		const tmrUpdWifi = setInterval(function () {
			$.get(apiLink + api.actions.API_WIFISCANSTATUS, function (data) {
				if (!data.scanDone) return;
				if (!data.wifi) {
					alert(i18next.t('p.ne.wifi.nnf'));
				} else {
					if (data.wifi.length > 0) {
						data.wifi.forEach((elem) => {
							let $row = $("<tr class='ssidSelector cursor-pointer' id='" + elem.ssid + "' >").appendTo("#wifiTable");
							$("<td>" + elem.ssid + "</td>").appendTo($row);
							let encryptType = "";
							switch (elem.secure) {
								case 2:
									encryptType = "WPA"
									break;

								case 3:
									encryptType = "WPA2"
									break;

								case 4:
									encryptType = "WPA2"
									break;

								case 5:
									encryptType = "WEP"
									break;

								case 7:
									encryptType = "OPEN"
									break;

								case 8:
									encryptType = "AUTO"
									break;

								default:
									break;
							}
							$("<td>" + encryptType + "</td>").appendTo($row);

							$("<td>" + elem.channel + "</td>").appendTo($row);
							$("<td>" + elem.rssi + "</td>").appendTo($row);

						});
						$("#wifiScanPreloader").addClass(classHide);
						clearInterval(tmrUpdWifi);
						$(".ssidSelector").click(function (elem) {
							$("#wifiSsid").val(elem.currentTarget.id);
							$("#wifiPass").val("");
							$("#collapseWifiPass").collapse("show");
						});
					} else {
						$("#wifiScanPreloader").addClass(classHide);
						$("#wifiScanButton").removeClass(classHide);
					}
				}
			});

		}, 2000);
	});
}

function isMobile() {
	return (((window.innerWidth <= 767)) && ('ontouchstart' in document.documentElement));
}

function EthEnbl(state) {
	state = !state;
	var dhcpEnabled = $("#ethDhcp").is(":checked");
	if (dhcpEnabled) {
		$("#ethDhcp").prop(disbl, state);
		$("#ethIp").prop(disbl, true);
		$("#ethMask").prop(disbl, true);
		$("#ethGate").prop(disbl, true);
		$("#ethDns1").prop(disbl, true);
		$("#ethDns2").prop(disbl, true);
	} else {
		$("#ethDhcp").prop(disbl, state);
		$("#ethIp").prop(disbl, state);
		$("#ethMask").prop(disbl, state);
		$("#ethGate").prop(disbl, state);
		$("#ethDns1").prop(disbl, state);
		$("#ethDns2").prop(disbl, state);
	}
}

function WifiEnbl(state) {
	$("#wifiEnbl").prop(chck, state);
	state = !state;
	var dhcpEnabled = $("#wifiDhcp").is(":checked");
	$("#wifiSsid").prop(disbl, state);
	$("#wifiPass").prop(disbl, state);
	$("#wifiMode").prop(disbl, state);
	$("#wifiPwr").prop(disbl, state);
	if (dhcpEnabled) {
		$("#wifiDhcp").prop(disbl, state);
		$("#wifiIp").prop(disbl, true);
		$("#wifiMask").prop(disbl, true);
		$("#wifiGate").prop(disbl, true);
		$("#wifiDns1").prop(disbl, true);
		$("#wifiDns2").prop(disbl, true);
	} else {
		$("#wifiDhcp").prop(disbl, state);
		$("#wifiIp").prop(disbl, state);
		$("#wifiMask").prop(disbl, state);
		$("#wifiGate").prop(disbl, state);
		$("#wifiDns1").prop(disbl, state);
		$("#wifiDns2").prop(disbl, state);
	}
}

function WifiDhcpDsbl(state) {
	$("#wifiIp").prop(disbl, state);
	$("#wifiMask").prop(disbl, state);
	$("#wifiGate").prop(disbl, state);
	$("#wifiDns1").prop(disbl, state);
	$("#wifiDns2").prop(disbl, state);
}

function EthDhcpDsbl(state) {
	$("#ethIp").prop(disbl, state);
	$("#ethMask").prop(disbl, state);
	$("#ethGate").prop(disbl, state);
	$("#ethDns1").prop(disbl, state);
	$("#ethDns2").prop(disbl, state);
}

function MqttInputDsbl(state) {
	$("#MqttServer").prop(disbl, state);
	$("#MqttPort").prop(disbl, state);
	$("#MqttUser").prop(disbl, state);
	$("#MqttPass").prop(disbl, state);
	$("#MqttTopic").prop(disbl, state);
	$("#MqttInterval").prop(disbl, state);
	$("#MqttDiscovery").prop(disbl, state);
	$("#mqttReconnect").prop(disbl, state);
}

function WgInputDsbl(state) {
	$("#wgLocalIP").prop(disbl, state);
	$("#wgLocalSubnet").prop(disbl, state);
	$("#wgLocalPort").prop(disbl, state);
	$("#wgLocalGateway").prop(disbl, state);
	$("#wgLocalPrivKey").prop(disbl, state);
	$("#wgEndAddr").prop(disbl, state);
	$("#wgEndPubKey").prop(disbl, state);
	$("#wgEndPort").prop(disbl, state);
	$("#wgAllowedIP").prop(disbl, state);
	$("#wgAllowedMask").prop(disbl, state);
	$("#wgMakeDefault").prop(disbl, state);
	$("#wgPreSharedKey").prop(disbl, state);
}

function HnInputDsbl(state) {
	$("#hnJoinCode").prop(disbl, state);
	$("#hnHostName").prop(disbl, state);
	$("#hnDashUrl").prop(disbl, state);
}

function SeqInputDsbl(state) {
	$("#webUser").prop(disbl, state);
	$("#webPass").prop(disbl, state);
	$('#div_show1').toggle(this.checked);
}

function SeqInputDsblFw(state) {
	$("#fwIp").prop(disbl, state);
	$('#div_show2').toggle(this.checked);
}

function readFile(event, file) {
	event.preventDefault();
	$("#config_file").val("Loading file: " + file);
	$.get(apiLink + api.actions.API_GET_FILE + "&filename=" + file, function (data) {
		$("#title").text(file);
		$("#filename").val(file);
		$("#config_file").val(data);
	});
}

function delFile(event, file) {
	event.preventDefault();
	$("#config_file").val("Deleted file: " + file);
	$.get(apiLink + api.actions.API_DEL_FILE + "&filename=" + file, function (data) { });
}

function logRefresh(ms) {
	var logUpd= setInterval(() => {
		$.get(apiLink + api.actions.API_GET_LOG, function (data) {
			if ($("#console").length) {
				$("#console").val(data);
			} else {
				clearInterval(logUpd);
			}
		});
	}, ms);
}

let languages = [
	{ value: "en", text: "🇬🇧 English" },
	{ value: "uk", text: "🇺🇦 Українська" },
	{ value: "zh", text: "🇨🇳 中文" },
	{ value: "es", text: "🇪🇸 Español" },
	{ value: "pt", text: "🇵🇹 Português" },
	{ value: "ru", text: "🇷🇺 Русский" },
	{ value: "fr", text: "🇫🇷 Français" },
	{ value: "de", text: "🇩🇪 Deutsch" },
	{ value: "ja", text: "🇯🇵 日本語" },
	{ value: "tr", text: "🇹🇷 Türkçe" },
	{ value: "it", text: "🇮🇹 Italiano" },
	{ value: "pl", text: "🇵🇱 Polski" },
	{ value: "cz", text: "🇨🇿 Čeština" }
];

$(document).ready(() => {
	const $dropdown = $("#langSel");
	$dropdown.empty();

	languages.forEach(({ text, value }) => $dropdown.append(new Option(text, value)));

	const savedLang = localStorage.getItem("selected-lang");
	const browserLang = navigator.language ? navigator.language.substring(0, 2) : navigator.userLanguage;
	let preferredLang = savedLang || (languages.some(lang => lang.value === browserLang) ? browserLang : 'en'); // 'en' как fallback

	$dropdown.val(preferredLang);
	$dropdown.on("change", function () {
		let selectedLang = $(this).val();
		localStorage.setItem("selected-lang", selectedLang);
		changeLanguage(selectedLang);
	});
});


function localize() {
	const elements = document.querySelectorAll('[data-i18n]');
	elements.forEach(element => {
		const keys = element.getAttribute('data-i18n').split(';');
		keys.forEach(key => {
			if (key.trim()) {
				if (key.includes('[')) {
					const parts = key.split('[');
					const attr = parts[1].slice(0, -1);
					element.setAttribute(attr, i18next.t(parts[0]));
				} else {
					const inputChild = element.querySelector('input');
					if (inputChild) {
						while (element.firstChild !== inputChild) {
							element.removeChild(element.firstChild);
						}
						const textNode = document.createTextNode(i18next.t(key));
						element.insertBefore(textNode, inputChild);
					} else {
						element.textContent = i18next.t(key);
					}
				}
			}
		});
	});
}


i18next
	.use(i18nextHttpBackend)
	.init({
		lng: 'en',
		fallbackLng: 'en',
		backend: {
			loadPath: '/lg/{{lng}}.json',
		},
	}, function (err, t) {

	});

function updateLocalizedContent() {
	localizeTitle(window.location.pathname);
	localize();
}


function changeLanguage(lng) {
	i18next.changeLanguage(lng, () => {
		updateLocalizedContent();
	});
}

function getURLParameter(sParam) {
	var sPageURL = window.location.search.substring(1);
	var sURLVariables = sPageURL.split('&');
	for (var i = 0; i < sURLVariables.length; i++) {
		var sParameterName = sURLVariables[i].split('=');
		if (sParameterName[0] == sParam) {
			return sParameterName[1];
		}
	}
}

function sub_esp(t) {
	t = t.value.split("\\\\");
	"" != t ? ($("#updButton").removeAttr("disabled"), localStorage.setItem("beta_feedback", 0)) : $("#updButton").prop(disbl, 1), document.getElementById("file-input").innerHTML = "   " + t[t.length - 1]
}

function sub_zb(t) {
	const file = t.files && t.files[0];
	$('#updButton_zb').prop(disbl, !file);
	$('#file-input_zb').text(file ? file.name : i18next.t('p.to.cf'));
}

async function fetchReleaseData() {
	var t = await fetch("https://docs.codm.de/tools/releases.php");
	if (t.ok) return await t.json();
	throw new Error("GitHub API request failed: " + t.statusText)
}

function handleClicks() {

	$("a.nav-link").click(function (e) { //handle navigation
		e.preventDefault();
		const url = $(this).attr("href");
		if (url == "/logout") {
			window.location = "/logout";
			return;
		}
		loadPage(url);
		$(".offcanvas-body  a.active").removeClass("active");
		$(this).addClass("active");
	});

	$(document).on('submit', '#esp_upload_form', function (e) {
		e.preventDefault();
		var formData = new FormData(this);
		modalConstructor("flashESP", formData);
	});

	$(document).on('click', '#upd_esp_git', function () {
		modalConstructor("flashESP");
	});

	$(document).on('click', '#info_esp_git', function () {
		modalConstructor("fetchGitReleases");
	});

	$(document).on('click', '#upd_zb_git', function () {
		modalConstructor("flashZB");
	});

	var lastEscTime = 0;
	var doublePressInterval = 300;

	$(document).on('keydown', function (e) {
		if (e.keyCode === 27) { // 27 - Esc
			var currentTime = new Date().getTime();
			if (currentTime - lastEscTime < doublePressInterval) {
				closeModal();
				lastEscTime = 0;
			} else {
				lastEscTime = currentTime;
			}
		}
	});

	const clockButton = document.getElementById('clock');
	clockButton.addEventListener('click', function () {
		const currentFormat = localStorage.getItem('clock_format_12h');
		const is12HourFormat = currentFormat === 'true';
		localStorage.setItem('clock_format_12h', !is12HourFormat);
	});
}

function handleMsg() {
	if (getURLParameter("msg")) {
		let msg_txt = "";
		msg_id = parseInt(decodeURI(getURLParameter("msg")));
		switch (msg_id) {
			case 1:
				msg_txt = "p.lo.mnl";
				break;
			case 2:
				msg_txt = "p.lo.mwc";

				break;
			case 3:
				msg_txt = "p.lo.mlo";
				break;
		}
		document.getElementById("messageTxt").setAttribute("data-i18n", msg_txt);
	}
}

function networkStatusUiInit() {
    const card = document.getElementById('networkStatusCard');
    if (!card) return;
    const tr = key => i18next.t('p.ne.live.' + key);
    let timer, pending = false;
    function load() {
        clearTimeout(timer);
        if (pending || document.getElementById('networkStatusCard') !== card) return;
        pending = true;
        $.getJSON('/api/network/status').done(function (data) {
            if (document.getElementById('networkStatusCard') !== card) return;
            $('#networkStatusError').addClass('d-none');
            const rows = $('#networkAddresses').empty();
            (data.interfaces || []).forEach(function (iface) {
                const row = $('<tr>').attr('data-interface', iface.id);
                $('<th>').attr('scope', 'row').text(tr(iface.id)).appendTo(row);
                $('<td>').text(tr(!iface.enabled ? 'disabled' : iface.connected ? 'connected' : 'disconnected')).appendTo(row);
                $('<td>').addClass('font-monospace text-break').text(iface.ipv4 || (iface.connected ? tr('waiting') : '—')).appendTo(row);
                const ipv6 = $('<td>').addClass('text-break').appendTo(row);
                (iface.ipv6 || []).forEach(function (ip) {
                    const line = $('<div>').appendTo(ipv6);
                    $('<span>').addClass('font-monospace').text(ip.address).appendTo(line);
                    if (ip.link_local) $('<small>').addClass('text-muted').text(' · ' + tr('linkLocal')).appendTo(line);
                });
                if (!ipv6.children().length) ipv6.text(iface.connected ? tr('waiting') : '—');
                rows.append(row);
            });
        }).fail(function () {
            if (document.getElementById('networkStatusCard') !== card) return;
            $('#networkAddresses').empty();
            $('#networkStatusError').removeClass('d-none').text(tr('failed'));
        }).always(function () {
            pending = false;
            if (document.getElementById('networkStatusCard') === card) timer = setTimeout(load, 5000);
        });
    }
    $('#networkStatusRefresh').on('click', load);
    load();
}

// Master/Satellite configuration on the Role page.
function backhaulUiInit() {
    const form = document.getElementById('backhaulConfig');
    if (!form) return;
    let token = '';
    let savedMode = 0;
    let selectedRole = null, roleSupported = false;
    let lastDebugState = '';
    let timer, pending = false, restarting = false;
    const tr = key => i18next.t('p.bh.' + key);
    function roleChanged() {
        const visible = roleSupported && selectedRole === 1;
        $('#backhaulSection').toggleClass('d-none', !visible);
        $(form).find('input,select,button').prop('disabled', !visible);
        return visible;
    }
    $('.selectable-card').on('click.backhaul', function () {
        selectedRole = this.classList.contains('zfs_coordinator') ? 1 : this.classList.contains('zfs_router') ? 2 : 3;
        roleChanged();
    });
    function message(text, error) {
        $('#bhMessage').removeClass('d-none alert-success alert-danger').addClass(error ? 'alert-danger' : 'alert-success').text(text);
    }
    function modeChanged() {
        const mode = Number($('#bhMode').val());
        $('#bhFields').toggleClass('d-none', mode === 0);
        $('#bhHost').prop('required', mode === 2);
        $('#bhHost').closest('.col-md-6').toggleClass('d-none', mode !== 2);
        $('#bhJoin').toggleClass('d-none', savedMode !== 2);
    }
    function status(data) {
        const st = data.status || {};
        const ieeeText = value => {
            const hex = String(value || '').replace(/^0x/i, '').replace(/:/g, '');
            return /^[0-9a-f]{16}$/i.test(hex) ? hex.toUpperCase().match(/../g).join(':') : String(value || '');
        };
        if (data.debug_mode) {
            // Only public diagnostics: never log the configuration response containing PSK.
            const event = {mode: data.mode, fault: st.fault || 'status_unavailable', radio: st.radio_revision || 0,
                required: st.radio_required || null, peers: st.peers_online || 0};
            const signature = JSON.stringify(event);
            if (signature !== lastDebugState) { console.info('[CZC]', event); lastDebugState = signature; }
        }
        $('#bhOwn').val(ieeeText(st.own_ieee));
        $('#bhPeers').empty();
        if (Number(data.mode) === 0) { $('#bhStatus').text(tr('faults.disabled')); return; }
        const peers = new Map();
        (st.peers || []).forEach(peer => {
            const ieee = ieeeText(peer.ieee);
            const previous = peers.get(ieee);
            if (!previous || peer.online || !previous.online) peers.set(ieee, peer);
        });
        peers.forEach((peer, ieee) => {
            const parts = [ieee];
            if (peer.ip) parts.push(peer.ip);
            parts.push(peer.online ? tr('connected') : tr('faults.' + peer.fault));
            $('<li>').text(parts.join(' · ')).appendTo('#bhPeers');
        });
        const parts = [];
        if (st.peer) {
            parts.push(Number(data.mode) === 1 ? tr('satellitesConnected') + ': ' + (st.peers_online || 1) + '/' + (st.peer_limit || 8) : tr('masterConnected'));
            if (st.af) parts.push('TLS / IPv' + st.af);
        } else if (st.fault === 'radio_revision' && st.radio_revision) {
            parts.push(tr('radioInstalled') + ': ' + st.radio_revision + '; ' + tr('radioRequired') + ': ' + (st.radio_required || '—'));
        } else if (!st.fault) parts.push(tr('faults.status_unavailable'));
        else if (['none', 'disabled', 'unconfigured', 'awaiting_peer'].includes(st.fault)) {
            parts.push(tr(Number(data.mode) === 1 ? 'waitingSatellites' : 'connectingMaster'));
        } else parts.push(tr('faults.' + st.fault));
        if (st.join && !['idle', 'joined'].includes(st.join) && st.join !== st.fault) parts.push(tr('joins.' + st.join));
        $('#bhStatus').text(parts.join(' · '));
    }
    function load(fill) {
        clearTimeout(timer);
        if (pending || restarting || document.getElementById('backhaulConfig') !== form) return;
        pending = true;
        return $.getJSON('/api/backhaul').done(function (data) {
            if (document.getElementById('backhaulConfig') !== form) return;
            token = data.token;
            savedMode = data.mode;
            roleSupported = data.role_supported === true;
            if (selectedRole === null) selectedRole = Number(data.radio_role);
            if (fill) {
                $('#bhMode').val(data.mode); $('#bhHost').val(data.peer_host);
                $('#bhPort').val(data.peer_port);
                $('#bhKey').val(data.psk || '');
            }
            modeChanged(); roleChanged(); status(data);
        }).fail(failure).always(function () {
            pending = false;
            if (!restarting && document.getElementById('backhaulConfig') === form) timer = setTimeout(() => load(false), 5000);
        });
    }
    function failure(xhr) {
        const reason = xhr.responseJSON && xhr.responseJSON.result;
        message(reason ? tr('errors.' + reason) : tr('requestFailed'), true);
    }
    function post(path, data) {
        return $.ajax({url: path, type: 'POST', contentType: 'application/json', dataType: 'json',
            headers: {'X-Backhaul-Token': token}, data: JSON.stringify(data || {})}).fail(failure);
    }
    $('#bhMode').on('change', modeChanged);
    $('#bhRefresh').on('click', () => load(false));
    $('#bhShowKey').on('click', function () { const field = document.getElementById('bhKey'); field.type = field.type === 'password' ? 'text' : 'password'; });
    $('#bhCopyKey').on('click', function () {
        const field = document.getElementById('bhKey');
        if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(field.value).then(() => message(tr('copied'), false)).catch(() => message(tr('copyFailed'), true));
        else {
            const old = field.type; field.type = 'text'; field.focus(); field.select();
            const ok = document.execCommand('copy'); field.type = old;
            message(tr(ok ? 'copied' : 'copyFailed'), !ok);
        }
    });
    $('#bhGenerateKey').on('click', function () {
        post('/api/backhaul/key').done(data => { $('#bhKey').val(data.psk); message(tr('generated'), false); });
    });
    $('#bhJoin').on('click', function () {
        $('#bhJoin').prop('disabled', true);
        post('/api/backhaul/join').done(() => { message(tr('joining'), false); setTimeout(() => load(false), 3000); }).always(() => $('#bhJoin').prop('disabled', false));
    });
    $(form).on('submit', function (event) {
        event.preventDefault();
        if (!roleChanged()) return;
        if (!form.reportValidity()) return;
        const data = {mode: Number($('#bhMode').val()), peer_host: $('#bhHost').val().trim(),
            psk: $('#bhKey').val().trim(), peer_port: Number($('#bhPort').val())};
        $('#bhSave').prop('disabled', true);
        post('/api/backhaul', data).done(() => {
            restarting = true; clearTimeout(timer);
            message(tr('saved'), false);
            setTimeout(() => window.location.reload(), 7000);
        }).fail(() => $('#bhSave').prop('disabled', false));
    });
    load(true);
}

$(document).on('submit', '#upload_form_zb', function (event) {
    event.preventDefault();
    const file = document.getElementById('file_zb').files[0];
    if (!file) return;
    modalConstructor('flashZBM');
    startZbFlash(file, 'coordinator');
});
