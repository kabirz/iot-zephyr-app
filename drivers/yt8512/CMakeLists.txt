zephyr_library()

# 复用上游 PHY 辅助头 (全 inline, 只依赖公共 API):
# phy_mii_cfg_link_autoneg() / phy_mii_set_bmcr_reg_autoneg_disabled()
zephyr_include_directories(${ZEPHYR_BASE}/drivers/ethernet/phy)

zephyr_library_sources_ifdef(CONFIG_PHY_MOTORCOMM_YT8512 yt8512.c)
