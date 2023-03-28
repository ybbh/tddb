import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf as pdf

from utils import util

linewidth = 2.2
markersize = 12

terminal_sdb_tb = util.json_attr_values(file='share/g7.4x_terminal_tb.json', attr_names=['tpm', 'terminals'],
                                        filter=lambda row: row['label'] == 'tight_bind')
terminal_sdb_lb_wan = util.json_attr_values(file='share/g7.4x_terminal_lb_wan.json', attr_names=['tpm', 'terminals'],
                                            filter=lambda row: row['label'] == 'loose_bind')
terminal_sdb_lb_lan = util.json_attr_values(file='share/g7.4x_terminal_lb_lan.json', attr_names=['tpm', 'terminals'],
                                            filter=lambda row: row['label'] == 'loose_bind')

tpm_terminal_tb = terminal_sdb_tb['tpm']
tpm_terminal_lb_wan = terminal_sdb_lb_wan['tpm']
tpm_terminal_lb_lan = terminal_sdb_lb_lan['tpm']
terminals = terminal_sdb_tb['terminals']

cache_sdb_tb = util.json_attr_values(file='share/g7.4x_cache_tb.json', attr_names=['tpm', 'percent_cached_tuple'],
                                     filter=lambda row: row['label'] == 'tight_bind')
cache_sdb_lb_lan = util.json_attr_values(file='share/g7.4x_cache_lb_lan.json',
                                         attr_names=['tpm', 'percent_cached_tuple'],
                                         filter=lambda row: row['label'] == 'loose_bind')
cache_sdb_lb_wan = util.json_attr_values(file='share/g7.4x_cache_lb_wan.json',
                                         attr_names=['tpm', 'percent_cached_tuple'],
                                         filter=lambda row: row['label'] == 'loose_bind')
tpm_cached_tb = cache_sdb_tb['tpm']
tpm_cached_lb_lan = cache_sdb_lb_lan['tpm']
tpm_cached_lb_wan = cache_sdb_lb_wan['tpm']
percent_cached = cache_sdb_tb['percent_cached_tuple']

fig = plt.figure()
ax = fig.subplots()

legend = ['DB-S-TB', 'DB-S-LB(WAN setting)', 'DB-S-LB(LAN setting)']
ax.plot(terminals, tpm_terminal_tb, '--o', color='tab:blue', label='DB-S-TB',
        linewidth=linewidth, markersize=markersize)
ax.plot(terminals, tpm_terminal_lb_wan, '-.v', color='tab:brown', label='DB-S-LB(WAN setting)',
        linewidth=linewidth, markersize=markersize)
ax.plot(terminals, tpm_terminal_lb_lan, '-.<', color='tab:orange', label='DB-S-LB(LAN setting)',
        linewidth=linewidth, markersize=markersize)
plt.xlabel("number of terminals")
plt.ylabel("TPM")
ax.legend(legend)
ax.set_xticks(terminals)
pp = pdf.PdfPages("fig_s_tpm_bind.pdf")
pp.savefig(fig, bbox_inches='tight')
pp.close()

ax.clear()

ax.plot(percent_cached, tpm_cached_tb, '--o', color='tab:blue', label='DB-S-TB',
        linewidth=linewidth, markersize=markersize)
ax.plot(percent_cached, tpm_cached_lb_wan, '-.v', color='tab:brown', label='DB-S-LB(WAN setting)',
        linewidth=linewidth, markersize=markersize)
ax.plot(percent_cached, tpm_cached_lb_lan, '-.<', color='tab:orange', label='DB-S-LB((LAN setting)',
        linewidth=linewidth, markersize=markersize)
plt.xlabel("percent of CCB cached rows")
plt.ylabel("TPM")
ax.legend(legend)
ax.set_xticks(percent_cached)
pp = pdf.PdfPages("fig_s_tpm_cache.pdf")
pp.savefig(fig, bbox_inches='tight')
pp.close()
