#include "Runtime/Resource/Asset/Data/DataTable.h"

// =============================================================================
// DataTableBase reflection registration. The base is abstract -- it does not
// itself produce loadable .zasset files; only its concrete user-derived
// wrappers (declared via DECLARE_DATA_TABLE) do. We still need it in the
// reflection registry because the editor walks `derived from DataTableBase`
// to enumerate "all data tables in this project" and to pick the right
// default Inspector view.
// =============================================================================

IMPLEMENT_REGISTER_CLASS(DataTableBase)
IMPLEMENT_OBJECT_SERAILIZE(DataTableBase)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(DataTableBase)

template<typename TransferFunction>
void DataTableBase::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    // m_SourceCsvRelpath is a tiny breadcrumb so the Inspector's "edit
    // source CSV" button can resolve the original file without consulting
    // the AssetRegistry. Emitted exactly once, here in the base, so the
    // per-wrapper Transfer() (see IMPLEMENT_DATA_TABLE) only has to forward
    // its row vector.
    transfer.Transfer(m_SourceCsvRelpath, "m_source_csv_relpath");
}
