#include "nau/mgpu/Common/pch.h"
#include "nau/mgpu/Common/GMesh.h"
#include <DirectXMesh.h>

#include <utility>
#include "nau/mgpu/Graphics/GMeshBuffer.h"
#include "nau/mgpu/Graphics/GCommandList.h"
#include "nau/mgpu/Common/NativeModel.h"


std::shared_ptr<NativeMesh> GMesh::GetMeshData() const
{
    return mesh;
}

D3D12_PRIMITIVE_TOPOLOGY GMesh::GetPrimitiveType() const
{
    return mesh->topology;
}

D3D12_VERTEX_BUFFER_VIEW* GMesh::GetVertexView() const
{
    return vertexBuffer->VertexBufferView();
}

D3D12_INDEX_BUFFER_VIEW* GMesh::GetIndexView() const
{
    return indexBuffer->IndexBufferView();
}

GMesh::GMesh(const std::shared_ptr<NativeMesh>& data, std::shared_ptr<GCommandList>& cmdList) : mesh(std::move(data))
{
    indexBuffer = std::make_shared<GMeshBuffer>(std::move(GMeshBuffer::CreateBuffer(
        cmdList, mesh->GetIndexes().data(), mesh->GetIndexSize(), mesh->GetIndexes().size(),
        mesh->GetName() + L" Indexes")));

    vertexBuffer = std::make_shared<GMeshBuffer>(std::move(GMeshBuffer::CreateBuffer(
        cmdList, mesh->GetVertexes().data(), mesh->GetVertexSize(), mesh->GetVertexes().size(),
        mesh->GetName() + L" Vertexes")));
}


void GMesh::Draw(const std::shared_ptr<GCommandList>& cmdList) const
{
    cmdList->SetVBuffer(0, 1, vertexBuffer->VertexBufferView());
    cmdList->SetIBuffer(indexBuffer->IndexBufferView());
    cmdList->SetPrimitiveTopology(mesh->topology);
    cmdList->DrawIndexed(mesh->GetIndexCount());
}

std::wstring GMesh::GetName() const
{
    return mesh->GetName();
}
